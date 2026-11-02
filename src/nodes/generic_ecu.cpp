/**
 * @file      generic_ecu.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     One binary that becomes any node of the communication matrix, chosen by environment.
 * @version   1.0
 * @date      2026-11-01
 * @copyright Copyright (c) 2026
 *
 * @details   @c gnss_ecu and @c hmi_ecu are written by hand against one matrix, which is the right
 *            shape for two nodes. Reproducing a nine-ECU bus that way would be nine files drifting
 *            from the matrix at the first slip. This node reads the generated runtime table instead
 *            and does what it says, so the matrix stays the only place a bus is described, and any
 *            matrix runs without writing code.
 *
 * @details   The deriving is not here: ::ids::nodes::PlanFor() turns the matrix into a plan, which
 *            is a value tests/test_matrix_plan.cpp asserts on without a bus. This file only EXECUTES
 *            the plan, which is the half that needs a stack.
 *
 * @details   WHAT THE MATRIX DETERMINES, and what it does not. From a capture one recovers who
 *            offers what, who subscribes to what, the payload SIZE and the publishing CADENCE. One
 *            does not recover the function that produced the bytes. So the content is a POLICY of
 *            this node, chosen by @c ECU_PAYLOAD, and never a claim about the reference bus. That
 *            knob is not a convenience: payload content is the first variable of the ladder in
 *            DISCUSSAO.md, and varying it is how one finds out whether the content byte models
 *            transfer between deployments.
 *
 * @details   One application per node, not one per service. ::ids::ecu::NodeRuntime explains why in
 *            detail; the short version is that a second vsomeip application with the same name gets
 *            SILENTLY renamed to @c name_0, which no generated configuration mentions. In the
 *            recovered matrix one consumer subscribes to three services, so this is not hypothetical.
 *
 * @details   Environment:
 *            - @c ECU_NODE              which node of the matrix to be; required.
 *            - @c ECU_PAYLOAD           content policy: @c counter (default), @c random, @c constant.
 *            - @c ECU_RUN_SECONDS       stop by itself after this long; zero, the default, waits for a signal.
 *            - @c ECU_READY_TIMEOUT_MS  how long to wait for the routing manager; default 10000.
 *            - @c ECU_REPORT_SECONDS    period of the counters line; default 5, zero silences it.
 */
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "src/libecu/NodeRuntime.hpp"
#include "src/nodes/MatrixPlan.hpp"

namespace
{

/// Set by the signal handler so the publishing loop can leave cleanly.
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

constexpr std::chrono::milliseconds kAvailabilityStep{5000}; ///< How long each attempt waits for a provider to appear.
constexpr std::chrono::milliseconds kIdleStep{200};          ///< Longest the loop sleeps with nothing due, so a signal is answered promptly.
constexpr std::uint32_t             kDefaultReadyMs = 10000U; ///< Default wait for the routing manager.
constexpr std::uint32_t             kDefaultReportS = 5U;     ///< Default period of the counters line.
constexpr std::uint8_t              kConstantByte   = 0x5AU;  ///< Filler of the @c constant policy.

/**
 * @brief How the payload bytes are produced.
 */
enum class PayloadPolicy
{
    Counter,  ///< Big-endian sequence counter, then zeros. Deterministic and easy to find in a capture.
    Random,   ///< Pseudo-random bytes, seeded per event so a run repeats.
    Constant  ///< The same byte everywhere.
};

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
 * @brief Read an environment variable as an unsigned number.
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
    const unsigned long v = std::strtoul(text.c_str(), &end, 10);
    return ((end != nullptr) && (*end == '\0')) ? static_cast<std::uint32_t>(v) : fallback;
}

/**
 * @brief Translate the ECU_PAYLOAD setting.
 * @param[in] text Value of the variable.
 * @return The policy it names; ::PayloadPolicy::Counter when the name is unknown.
 */
PayloadPolicy PoliticaDe(const std::string &text)
{
    PayloadPolicy p = PayloadPolicy::Counter;
    if (text == "random")
    {
        p = PayloadPolicy::Random;
    }
    else if (text == "constant")
    {
        p = PayloadPolicy::Constant;
    }
    else
    {
        /* Counter, which is also what an unknown name falls back to. */
    }
    return p;
}

/**
 * @brief Publishing state of one planned event.
 */
struct Publicacao
{
    ids::nodes::EventPlan                 plan;         ///< What the matrix says about it.
    std::chrono::steady_clock::time_point due;          ///< When it is next due.
    std::chrono::microseconds             period{0};    ///< Cadence, from the plan.
    std::uint32_t                         counter = 0U; ///< Sequence number put at the front of the payload.
    std::mt19937                          rng;          ///< Generator of the @c random policy, seeded per event.
    std::uint64_t                         sent    = 0U; ///< How many notifications went out.
};

/**
 * @brief Fill a payload according to the policy.
 * @param[in]     policy What kind of content to produce.
 * @param[in,out] pub    Event being published; its counter and generator advance.
 * @return The bytes to send.
 */
std::vector<std::uint8_t> MontaPayload(PayloadPolicy policy, Publicacao &pub)
{
    std::vector<std::uint8_t> bytes(pub.plan.bytes, 0U);

    if (policy == PayloadPolicy::Random)
    {
        std::uniform_int_distribution<unsigned> dist(0U, 255U);
        for (std::size_t i = 0U; i < bytes.size(); ++i)
        {
            bytes[i] = static_cast<std::uint8_t>(dist(pub.rng));
        }
    }
    else if (policy == PayloadPolicy::Constant)
    {
        for (std::size_t i = 0U; i < bytes.size(); ++i)
        {
            bytes[i] = kConstantByte;
        }
    }
    else
    {
        /* Counter: a big-endian sequence number up front, zeros after it. A subscriber can check
         * that notifications arrive in order, and the field is easy to find in a capture. */
        for (std::size_t i = 0U; (i < 4U) && (i < bytes.size()); ++i)
        {
            const unsigned shift = static_cast<unsigned>(8U * (3U - i));
            bytes[i]             = static_cast<std::uint8_t>((pub.counter >> shift) & 0xFFU);
        }
    }

    ++pub.counter;
    return bytes;
}

/**
 * @brief Name of a transport, for the log lines.
 * @param[in] t Transport to name.
 * @return A short word. @c "indefinido" is the one that matters: it means the stack chooses, and
 *         that is what gave 65% TCP where the reference dataset has 99,1%.
 */
const char *NomeDoTransporte(ids::ecu::Transport t)
{
    const char *nome = "indefinido";
    if (t == ids::ecu::Transport::Reliable)
    {
        nome = "tcp";
    }
    else if (t == ids::ecu::Transport::Unreliable)
    {
        nome = "udp";
    }
    else if (t == ids::ecu::Transport::Both)
    {
        nome = "tcp+udp";
    }
    else
    {
        /* Unspecified. */
    }
    return nome;
}

/**
 * @brief Print the nodes the table knows, for when ECU_NODE names none of them.
 * @return None.
 */
void ListaNos()
{
    const ids::matrix::table::Table<ids::matrix::table::NodeDesc> nos = ids::matrix::table::Nodes();
    std::printf("[generic_ecu] nos da matriz:");
    for (std::size_t i = 0U; i < nos.count; ++i)
    {
        std::printf(" %s", nos.items[i].name);
    }
    std::printf("\n");
}

} // namespace

int main()
{
    /* Line buffering, so docker logs show the lines as they happen instead of in blocks when the
     * process ends. A node killed mid-run would otherwise appear to have printed nothing, which has
     * already cost two runs on this bench. */
    static_cast<void>(std::setvbuf(stdout, nullptr, _IOLBF, 0));

    const std::string nome = Env("ECU_NODE", "");
    if (nome.empty())
    {
        std::printf("[generic_ecu] ERRO: defina ECU_NODE com o nome do no na matriz\n");
        ListaNos();
        return 2;
    }
    else
    {
        /* A name was given. */
    }

    const ids::nodes::NodePlan plano = ids::nodes::PlanFor(nome.c_str());
    if (plano.node == nullptr)
    {
        std::printf("[generic_ecu] ERRO: %s\n", plano.problem.c_str());
        ListaNos();
        return 2;
    }
    else if (!plano.problem.empty())
    {
        std::printf("[%s] ERRO na matriz: %s\n", plano.node->name, plano.problem.c_str());
        return 2;
    }
    else
    {
        /* A usable plan. */
    }

    const std::string   politicaNome = Env("ECU_PAYLOAD", "counter");
    const PayloadPolicy politica     = PoliticaDe(politicaNome);
    const std::uint32_t runSegs      = EnvNumber("ECU_RUN_SECONDS", 0U);
    const std::uint32_t prontoMs     = EnvNumber("ECU_READY_TIMEOUT_MS", kDefaultReadyMs);
    const std::uint32_t reportSegs   = EnvNumber("ECU_REPORT_SECONDS", kDefaultReportS);

    std::printf("[%s] app \"%s\", ip %s, publica %zu evento(s), assina %zu servico(s), payload %s\n", plano.node->name,
                plano.node->appName, plano.node->ip, plano.publishes.size(), plano.consumes.size(), politicaNome.c_str());

    ara::core::Result<std::unique_ptr<ids::ecu::NodeRuntime>> criado = ids::ecu::NodeRuntime::Create(plano.node->appName);
    if (!criado.HasValue())
    {
        std::printf("[%s] ERRO criando o runtime: %s\n", plano.node->name, criado.Error().Message());
        return 1;
    }
    else
    {
        /* Application created and initialised. */
    }
    ids::ecu::NodeRuntime &runtime = *criado.Value();

    /* Os handlers de sinal vao DEPOIS do Create(), e a ordem e o ponto.
     *
     * Compilado com VSOMEIP_ENABLE_SIGNAL_HANDLING, o init() do vsomeip -- que roda
     * dentro do Create() -- registra um boost::asio::signal_set para SIGINT e
     * SIGTERM cujo unico efeito e chamar application::stop(). Esse registro usa
     * sigaction e SOBRESCREVE um std::signal instalado antes dele.
     *
     * Instalar antes custou uma rodada: `docker kill --signal=SIGTERM` derrubava a
     * PILHA e deixava este laco girando, com o notify() falhando em silencio e o
     * contador de enviados subindo sobre nada. O container ficava `running` e o
     * resumo final nunca saia. Instalando depois, o sinal chega aqui, o laco sai e
     * o destrutor desce a pilha na ordem. */
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    /* ---- declaracao: tudo antes do Start(), que e o que a pilha exige ---- */

    for (std::size_t i = 0U; i < plano.node->provideCount; ++i)
    {
        const ids::matrix::table::ServiceDesc *const svc = ids::matrix::table::FindService(plano.node->provides[i]);
        const ara::core::Result<void>                declarado = runtime.ProvideService(svc->id, svc->instance);
        if (!declarado.HasValue())
        {
            std::printf("[%s] ERRO declarando %s: %s\n", plano.node->name, plano.node->provides[i], declarado.Error().Message());
            return 1;
        }
        else
        {
            /* Instance declared; the plan guarantees FindService() found it. */
        }
    }

    std::vector<Publicacao> publicacoes;
    for (std::size_t i = 0U; i < plano.publishes.size(); ++i)
    {
        const ids::nodes::EventPlan &ep = plano.publishes[i];

        /* Cycle zero on purpose. The cyclic update of vsomeip would resend the SAME payload, and
         * constant content is not what the reference bus carries; the loop below sends a fresh
         * payload at the recovered cadence instead. */
        const ara::core::Result<void> ofertado =
            runtime.ProvideEvent(ep.service, ep.instance, ep.notifier, ids::ecu::EventgroupIds(ep.eventgroups.data(), ep.eventgroups.size()),
                                 ep.isField, std::chrono::milliseconds::zero(), ep.transport);
        if (!ofertado.HasValue())
        {
            std::printf("[%s] ERRO declarando evento 0x%04X: %s\n", plano.node->name, static_cast<unsigned>(ep.notifier),
                        ofertado.Error().Message());
            return 1;
        }
        else
        {
            /* Event declared. */
        }

        Publicacao pub;
        pub.plan   = ep;
        pub.period = std::chrono::microseconds(static_cast<std::int64_t>(ep.cycleMs) * 1000);
        /* Seeded from the identifiers, so two runs of the same node produce the same bytes and two
         * different events never produce the same stream. */
        pub.rng.seed(static_cast<std::uint32_t>((static_cast<std::uint32_t>(ep.service) << 16U) | ep.notifier));
        publicacoes.push_back(pub);

        std::printf("[%s]   publica %s 0x%04X, %zu B cada %u ms, %zu eventgroup(s), %s\n", plano.node->name, ep.serviceName.c_str(),
                    static_cast<unsigned>(ep.notifier), ep.bytes, static_cast<unsigned>(ep.cycleMs), ep.eventgroups.size(),
                    NomeDoTransporte(ep.transport));
    }

    for (std::size_t i = 0U; i < plano.consumes.size(); ++i)
    {
        const ids::nodes::ConsumePlan &cp     = plano.consumes[i];
        const ara::core::Result<void>  pedido = runtime.ConsumeService(cp.service, cp.instance);
        if (!pedido.HasValue())
        {
            std::printf("[%s] ERRO declarando consumo de %s: %s\n", plano.node->name, cp.serviceName.c_str(), pedido.Error().Message());
            return 1;
        }
        else
        {
            /* Instance requested at Start(). */
        }
    }

    /* One counter per (service, event) received. Written from the stack thread and read from this
     * one, so both sides take the lock. */
    std::map<std::uint32_t, std::uint64_t> recebidos;
    std::mutex                             recebidosMutex;
    runtime.SetReceiveHandler([&recebidos, &recebidosMutex](ids::ecu::ServiceId service, ids::ecu::InstanceId, ids::ecu::EventId notifier,
                                                            ids::ecu::ByteView payload) {
        static_cast<void>(payload);
        const std::uint32_t         chave = (static_cast<std::uint32_t>(service) << 16U) | notifier;
        std::lock_guard<std::mutex> lock(recebidosMutex);
        ++recebidos[chave];
    });

    /* ---- subida ---- */

    const ara::core::Result<void> iniciado = runtime.Start();
    if (!iniciado.HasValue())
    {
        std::printf("[%s] ERRO subindo a pilha: %s\n", plano.node->name, iniciado.Error().Message());
        return 1;
    }
    else
    {
        /* Stack coming up on its own thread. */
    }

    const ara::core::Result<void> registrado = runtime.WaitUntilRegistered(std::chrono::milliseconds(prontoMs));
    if (!registrado.HasValue())
    {
        std::printf("[%s] ERRO: %s\n", plano.node->name, registrado.Error().Message());
        return 1;
    }
    else
    {
        std::printf("[%s] registrado no routing manager\n", plano.node->name);
    }

    if (plano.node->provideCount > 0U)
    {
        const ara::core::Result<void> ofertado = runtime.Offer();
        if (!ofertado.HasValue())
        {
            std::printf("[%s] ERRO ofertando: %s\n", plano.node->name, ofertado.Error().Message());
            return 1;
        }
        else
        {
            std::printf("[%s] %zu servico(s) ofertado(s)\n", plano.node->name, plano.node->provideCount);
        }
    }
    else
    {
        /* A pure consumer offers nothing. */
    }

    /* Subscribing only makes sense once Service Discovery found the provider. In a nine-node
     * bring-up a provider may well come up after us, so this waits in steps and keeps saying what it
     * is waiting for, instead of failing on one timeout. */
    for (std::size_t i = 0U; (i < plano.consumes.size()) && (!g_stop.load()); ++i)
    {
        const ids::nodes::ConsumePlan &cp = plano.consumes[i];

        bool disponivel = false;
        while ((!disponivel) && (!g_stop.load()))
        {
            disponivel = runtime.WaitUntilAvailable(cp.service, cp.instance, kAvailabilityStep).HasValue();
            if (!disponivel)
            {
                std::printf("[%s] esperando %s (0x%04X) aparecer...\n", plano.node->name, cp.serviceName.c_str(),
                            static_cast<unsigned>(cp.service));
            }
            else
            {
                /* Found. */
            }
        }
        if (!disponivel)
        {
            break;
        }
        else
        {
            /* Available: subscribe to exactly what the plan names. */
        }

        for (std::map<ids::ecu::EventId, std::vector<ids::ecu::EventgroupId>>::const_iterator it = cp.subscriptions.begin();
             it != cp.subscriptions.end(); ++it)
        {
            const ara::core::Result<void> assinado =
                runtime.Subscribe(cp.service, cp.instance, it->first, ids::ecu::EventgroupIds(it->second.data(), it->second.size()),
                                  cp.transport);
            if (!assinado.HasValue())
            {
                std::printf("[%s] ERRO assinando 0x%04X de %s: %s\n", plano.node->name, static_cast<unsigned>(it->first),
                            cp.serviceName.c_str(), assinado.Error().Message());
            }
            else
            {
                std::printf("[%s]   assina %s 0x%04X em %zu eventgroup(s), %s\n", plano.node->name, cp.serviceName.c_str(),
                            static_cast<unsigned>(it->first), it->second.size(), NomeDoTransporte(cp.transport));
            }
        }
    }

    /* ---- laco: publica cada evento na sua cadencia ---- */

    const std::chrono::steady_clock::time_point inicio = std::chrono::steady_clock::now();
    for (std::size_t i = 0U; i < publicacoes.size(); ++i)
    {
        publicacoes[i].due = inicio;
    }
    std::chrono::steady_clock::time_point proximoReport = inicio + std::chrono::seconds(reportSegs);

    while (!g_stop.load())
    {
        const std::chrono::steady_clock::time_point agora = std::chrono::steady_clock::now();

        if ((runSegs > 0U) && ((agora - inicio) >= std::chrono::seconds(runSegs)))
        {
            std::printf("[%s] %u s cumpridos\n", plano.node->name, runSegs);
            break;
        }
        else
        {
            /* Either it runs until a signal, or there is time left. */
        }

        /* A pilha pode cair sob os pes do laco, e o notify() nao reclama: devolve
         * sucesso e nada vai para o barramento. Sem esta checagem o no seguiria
         * contando envios sobre uma pilha morta, que foi exatamente o sintoma que a
         * ordem dos sinais acima produzia. */
        if (!runtime.IsRegistered())
        {
            std::printf("[%s] a pilha saiu do ar (deixou de estar registrada); encerrando\n", plano.node->name);
            break;
        }
        else
        {
            /* Still registered. */
        }

        /* Every event that is due goes out now, and is rescheduled from its PREVIOUS due time, so a
         * late wake-up does not make the cadence drift away from the recovered one. */
        std::chrono::steady_clock::time_point proximo = agora + kIdleStep;
        for (std::size_t i = 0U; i < publicacoes.size(); ++i)
        {
            Publicacao &pub = publicacoes[i];
            if (pub.period.count() == 0)
            {
                continue;
            }
            else
            {
                /* Has a cadence. */
            }

            if (pub.due <= agora)
            {
                const std::vector<std::uint8_t> bytes = MontaPayload(politica, pub);
                const ara::core::Result<void>   enviado =
                    runtime.Send(pub.plan.service, pub.plan.instance, pub.plan.notifier, ids::ecu::ByteView(bytes.data(), bytes.size()));
                if (!enviado.HasValue())
                {
                    std::printf("[%s] ERRO enviando 0x%04X: %s\n", plano.node->name, static_cast<unsigned>(pub.plan.notifier),
                                enviado.Error().Message());
                }
                else
                {
                    ++pub.sent;
                }

                pub.due += pub.period;
                /* One step is not enough when the process was held up for longer than the period.
                 * Catching up by resending would be a burst the reference bus does not have, so the
                 * missed slots are dropped and the phase is kept. */
                if (pub.due <= agora)
                {
                    pub.due = agora + pub.period;
                }
                else
                {
                    /* Still ahead. */
                }
            }
            else
            {
                /* Not due yet. */
            }

            if (pub.due < proximo)
            {
                proximo = pub.due;
            }
            else
            {
                /* Another event comes first. */
            }
        }

        if ((reportSegs > 0U) && (agora >= proximoReport))
        {
            std::uint64_t enviados = 0U;
            for (std::size_t i = 0U; i < publicacoes.size(); ++i)
            {
                enviados += publicacoes[i].sent;
            }
            std::uint64_t total = 0U;
            {
                std::lock_guard<std::mutex> lock(recebidosMutex);
                for (std::map<std::uint32_t, std::uint64_t>::const_iterator it = recebidos.begin(); it != recebidos.end(); ++it)
                {
                    total += it->second;
                }
            }
            std::printf("[%s] enviados %llu, recebidos %llu\n", plano.node->name, static_cast<unsigned long long>(enviados),
                        static_cast<unsigned long long>(total));
            proximoReport = agora + std::chrono::seconds(reportSegs);
        }
        else
        {
            /* Not time to report. */
        }

        /* A pure consumer has nothing due, so it waits a step at a time and leaves on the signal. */
        const std::chrono::steady_clock::time_point limite = agora + kIdleStep;
        std::this_thread::sleep_until((proximo < limite) ? proximo : limite);
    }

    std::uint64_t enviados = 0U;
    for (std::size_t i = 0U; i < publicacoes.size(); ++i)
    {
        enviados += publicacoes[i].sent;
        std::printf("[%s] 0x%04X/0x%04X: %llu enviados\n", plano.node->name, static_cast<unsigned>(publicacoes[i].plan.service),
                    static_cast<unsigned>(publicacoes[i].plan.notifier), static_cast<unsigned long long>(publicacoes[i].sent));
    }
    {
        std::lock_guard<std::mutex> lock(recebidosMutex);
        for (std::map<std::uint32_t, std::uint64_t>::const_iterator it = recebidos.begin(); it != recebidos.end(); ++it)
        {
            std::printf("[%s] 0x%04X/0x%04X: %llu recebidos\n", plano.node->name, static_cast<unsigned>(it->first >> 16U),
                        static_cast<unsigned>(it->first & 0xFFFFU), static_cast<unsigned long long>(it->second));
        }
    }
    std::printf("[%s] encerrando, %llu enviados\n", plano.node->name, static_cast<unsigned long long>(enviados));
    return 0;
}
