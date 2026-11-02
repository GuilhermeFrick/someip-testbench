/**
 * @file      ids_sensor_node.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Intrusion detection node: captures the bus, classifies every frame and reports
 *            security events to the IdsM.
 * @version   1.0
 * @date      2026-11-01
 * @copyright Copyright (c) 2026
 *
 * @details   This is the node that makes the component a sensor rather than a library. It is also
 *            the first node that does NOT offer or consume a service: it listens. That is why it
 *            has neither a Skeleton nor a Proxy, and why its vsomeip configuration exists only for
 *            a future in which it reports over SOME/IP.
 *
 * @details   IT MUST JOIN THE MULTICAST GROUPS, and this is the whole reason the node is not three
 *            lines long. The Docker bridge delivers multicast only to whoever joined the group
 *            over IGMP, and a passive capture does not join anything. A monitor that skips this
 *            captures the unicast traffic and ZERO Service Discovery packets, with no error to
 *            show for it, which is the failure that cost the most time in platform/sd-smoke. The
 *            groups come from the generated header, so adding an eventgroup with its own multicast
 *            address to the matrix is enough to make this node join it too.
 *
 * @details   The loop is NOT ::ids::Sensor::Run(). That one drains a source until it yields
 *            nothing, which for a replay means the end of the file and for a live capture means
 *            only that nothing arrived within the read timeout. Here the empty Optional is the
 *            normal quiet case, and what ends the loop is the shutdown flag.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "build/cfg/CommMatrix.hpp"
#include "src/ids_sensor/Sensor.hpp"

namespace
{

/// Set by the signal handler so the capture loop can leave cleanly.
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

constexpr std::uint64_t kReportEvery = 100000U; ///< Frames between progress lines.

/**
 * @brief Value of an environment variable, or a default.
 * @param[in] name       Variable to read.
 * @param[in] porPadrao  Value to use when the variable is unset or empty.
 * @return The value to use.
 */
std::string Env(const char *name, const char *porPadrao)
{
    const char *const valor = std::getenv(name);
    if ((valor == nullptr) || (valor[0] == '\0'))
    {
        return std::string(porPadrao);
    }
    return std::string(valor);
}

/**
 * @brief Holds the multicast memberships open for the life of the node.
 * @details The membership lasts as long as the socket, so the sockets are kept rather than opened
 *          and closed. They carry no traffic: joining is what makes the bridge forward the group
 *          to this container, and libpcap is what reads it.
 */
class MulticastMemberships final
{
  public:
    /**
     * @brief Construct with no memberships.
     * @return None.
     */
    MulticastMemberships() = default;

    /**
     * @brief Leave every group and close the sockets.
     * @return None.
     */
    ~MulticastMemberships()
    {
        for (std::size_t i = 0U; i < m_sockets.size(); ++i)
        {
            static_cast<void>(::close(m_sockets[i]));
        }
    }

    MulticastMemberships(const MulticastMemberships &) = delete;            ///< Owns descriptors.
    MulticastMemberships &operator=(const MulticastMemberships &) = delete; ///< Owns descriptors.

    /**
     * @brief Join one group.
     * @param[in] grupo    Dotted multicast address.
     * @param[in] porta    Port to bind, which has to match the one the traffic uses.
     * @param[in] interface Dotted address of the local interface to join on.
     * @return Whether the group was joined.
     */
    bool Join(const char *grupo, std::uint16_t porta, const std::string &interface)
    {
        const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
        {
            return false;
        }

        /* SO_REUSEADDR so that binding the Service Discovery port does not collide
         * with a vsomeip stack running in the same container. */
        const int sim = 1;
        static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &sim, sizeof(sim)));

        struct sockaddr_in endereco;
        std::memset(&endereco, 0, sizeof(endereco));
        endereco.sin_family      = AF_INET;
        endereco.sin_addr.s_addr = htonl(INADDR_ANY);
        endereco.sin_port        = htons(porta);
        if (::bind(fd, reinterpret_cast<struct sockaddr *>(&endereco), sizeof(endereco)) < 0)
        {
            static_cast<void>(::close(fd));
            return false;
        }

        struct ip_mreq pedido;
        std::memset(&pedido, 0, sizeof(pedido));
        if (::inet_pton(AF_INET, grupo, &pedido.imr_multiaddr) != 1)
        {
            static_cast<void>(::close(fd));
            return false;
        }
        /* The interface is named by ADDRESS, not by name: the matrix fixes the node
         * address, so the right interface is the one carrying it. */
        if (::inet_pton(AF_INET, interface.c_str(), &pedido.imr_interface) != 1)
        {
            pedido.imr_interface.s_addr = htonl(INADDR_ANY);
        }

        if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &pedido, sizeof(pedido)) < 0)
        {
            static_cast<void>(::close(fd));
            return false;
        }

        m_sockets.push_back(fd);
        return true;
    }

    /**
     * @brief How many groups are joined.
     * @return Number of live memberships.
     */
    std::size_t Count() const noexcept
    {
        return m_sockets.size();
    }

  private:
    std::vector<int> m_sockets; ///< One socket per membership, held open.
};

} // namespace

int main()
{
    namespace mtx = ids::matrix;

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    /* Saida em modo LINHA. Quando a saida e um pipe, como no `docker logs`, a
     * libc usa buffer cheio e nada aparece ate o processo terminar ou o buffer
     * encher -- num no que roda por minutos, isso faz a telemetria de progresso
     * nao existir justamente enquanto ela seria util. */
    static_cast<void>(std::setvbuf(stdout, nullptr, _IOLBF, 0));

    const std::string iface     = Env("IDS_INTERFACE", "eth0");
    /* Empty by default: a filter is part of the measurement, so it is never applied
     * without someone asking. On a bench bus "ip and (tcp or udp)" already drops the
     * infrastructure chatter that the reference bus did not have. */
    const std::string filtro    = Env("IDS_FILTER", "");
    const std::string ipLocal   = Env("IDS_LOCAL_IP", mtx::IdsSensor::Ip());
    const std::string dirCfg    = Env("IDS_ARTEFACTS", "build/cfg");
    const std::string prefixo   = Env("IDS_EVENT_PREFIX", "ids_sensor/SecurityEvents");

    std::printf("[ids_sensor] interface %s, address %s, artefacts in %s\n", iface.c_str(), ipLocal.c_str(),
                dirCfg.c_str());
    /* The filter is part of the MEASUREMENT: what it drops never reaches the flow
     * state, so two runs with different filters are not comparable. It is printed
     * next to the result rather than only read from the environment. */
    std::printf("[ids_sensor] filter %s\n", filtro.empty() ? "(none: everything on the wire)" : filtro.c_str());

    /* ------------------------------------------------- 1. join the groups */
    /* BEFORE capturing. A membership taken afterwards would miss whatever the
     * bridge dropped in the meantime, and the Service Discovery offers are
     * cyclic but the first ones are the interesting ones. */
    MulticastMemberships grupos;
    for (std::size_t i = 0U; i < mtx::MulticastGroups::Count(); ++i)
    {
        const char *const grupo = mtx::MulticastGroups::Address(i);
        const std::uint16_t porta = mtx::MulticastGroups::Port(i);
        if (grupos.Join(grupo, porta, ipLocal))
        {
            std::printf("[ids_sensor] joined %s:%u  (%s)\n", grupo, static_cast<unsigned>(porta),
                        mtx::MulticastGroups::Purpose(i));
        }
        else
        {
            /* Not fatal, and said out loud. Without the membership the Service
             * Discovery simply never reaches this container, and the capture looks
             * healthy while missing an entire layer of the traffic. */
            std::printf("[ids_sensor] WARNING could not join %s:%u (%s): this node will NOT see that "
                        "multicast, and Service Discovery frames will be missing from the capture\n",
                        grupo, static_cast<unsigned>(porta), std::strerror(errno));
        }
    }
    if (grupos.Count() == 0U)
    {
        std::printf("[ids_sensor] WARNING no multicast group joined; check --cap-add NET_ADMIN and the "
                    "route to %s\n", mtx::Network::MulticastRoute());
    }

    /* --------------------------------------------------- 2. the artefacts */
    ara::core::Result<std::unique_ptr<ids::ByteModels>> modelos =
        ids::ByteModels::Create(dirCfg + "/bytemodels.bin");
    if (!modelos.HasValue())
    {
        std::printf("[ids_sensor] ERROR byte models: %s\n", modelos.Error().Message());
        return 1;
    }

    ara::core::Result<ids::Normalizer> norm = ids::Normalizer::Create(dirCfg + "/minmax.bin");
    if (!norm.HasValue())
    {
        std::printf("[ids_sensor] ERROR normalisation bounds: %s\n", norm.Error().Message());
        return 1;
    }

    /* The compiled model comes first; the node falls back to the tables so that a
     * build without the TL2cgen step still runs. Which engine is active is printed,
     * because a measurement taken on the wrong engine is worse than no measurement. */
    std::unique_ptr<ids::IClassifier>                         motor;
    ara::core::Result<std::unique_ptr<ids::Tl2cgenClassifier>> gerado = ids::Tl2cgenClassifier::Create();
    if (gerado.HasValue())
    {
        motor = std::move(gerado.Value());   /* std::move no VALOR, nao no Result: Value() nao tem sobrecarga rvalue. */
    }
    else
    {
        ara::core::Result<std::unique_ptr<ids::CompactClassifier>> tabelas =
            ids::CompactClassifier::Create(dirCfg + "/tables.bin");
        if (!tabelas.HasValue())
        {
            std::printf("[ids_sensor] ERROR no usable engine: generated model says \"%s\", tables say \"%s\"\n",
                        gerado.Error().Message(), tabelas.Error().Message());
            return 1;
        }
        motor = std::move(tabelas.Value());
    }
    std::printf("[ids_sensor] inference engine: %s\n", motor->Name());

    ids::StreamerConfig cfgStreamer;
    ara::core::Result<std::unique_ptr<ids::FeatureStreamer>> extrator =
        ids::FeatureStreamer::Create(*modelos.Value(), cfgStreamer);
    if (!extrator.HasValue())
    {
        std::printf("[ids_sensor] ERROR extractor: %s\n", extrator.Error().Message());
        return 1;
    }

    ara::core::Result<std::unique_ptr<ids::IdsmDispatcher>> idsm = ids::IdsmDispatcher::Create(prefixo);
    if (!idsm.HasValue())
    {
        std::printf("[ids_sensor] ERROR security events: %s\n", idsm.Error().Message());
        std::printf("[ids_sensor]       ARA_IDSM_CONFIGURATION must name the generated "
                    "build/cfg/idsm-events.txt\n");
        return 1;
    }

    /* ------------------------------------------------------ 3. the capture */
    ara::core::Result<std::unique_ptr<ids::LiveSource>> fonte = ids::LiveSource::Create(iface, filtro);
    if (!fonte.HasValue())
    {
        std::printf("[ids_sensor] ERROR capture on %s: %s\n", iface.c_str(), fonte.Error().Message());
        std::printf("[ids_sensor]       live capture needs CAP_NET_RAW\n");
        return 1;
    }

    ids::SomeipParser parser;
    ids::Sensor       sensor(parser, *extrator.Value(), norm.Value(), *motor, *idsm.Value());

    std::printf("[ids_sensor] capturing; stop with SIGINT\n");

    /* THE LIVE LOOP, which is not Sensor::Run(). An empty Optional here means "no
     * frame within the read timeout", the normal case on a quiet bus, and the loop
     * uses exactly that moment to notice the shutdown flag. */
    std::uint64_t ultimoRelato = 0U;
    while (!g_stop.load())
    {
        const ara::core::Optional<ids::Frame> quadro = fonte.Value()->Next();
        if (quadro.has_value())
        {
            sensor.OnFrame(quadro.value());
        }
        else
        {
            /* Quiet bus. The timeout already paced this loop, so there is nothing
             * to sleep on. */
        }

        const ids::StageTiming &t = sensor.Timing();
        if ((t.packets - ultimoRelato) >= kReportEvery)
        {
            ultimoRelato = t.packets;
            std::printf("[ids_sensor] %llu packets, %llu dropped, %llu security events\n",
                        static_cast<unsigned long long>(t.packets), static_cast<unsigned long long>(t.malformed),
                        static_cast<unsigned long long>(t.events));
        }
    }

    /* ------------------------------------------------------ 4. the verdict */
    const ids::StageTiming &t = sensor.Timing();
    std::printf("\n[ids_sensor] stopped\n");
    std::printf("[ids_sensor] packets         : %llu\n", static_cast<unsigned long long>(t.packets));
    std::printf("[ids_sensor] dropped by parser: %llu\n", static_cast<unsigned long long>(t.malformed));
    std::printf("[ids_sensor] security events : %llu\n", static_cast<unsigned long long>(t.events));
    if (t.packets > 0U)
    {
        const double n = static_cast<double>(t.packets);
        std::printf("[ids_sensor] per packet, us : parse %.3f  features %.3f  classify %.3f  decide %.3f\n",
                    (t.parseUs / n), (t.featureUs / n), (t.classifyUs / n), (t.decideUs / n));
        std::printf("[ids_sensor] (five clock readings per frame are included in those; the clean\n");
        std::printf("[ids_sensor]  per-packet figure comes from the latency bench)\n");
    }
    return 0;
}
