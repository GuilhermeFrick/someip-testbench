/**
 * @file      CtrlChannel.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Control channel of the injector: text commands over UDP on the @c ctrl network.
 * @version   1.0
 * @date      2026-11-02
 * @copyright Copyright (c) 2026
 *
 * @details   Phase D of docs/PLANO.md: switch an injector on and off and tune it while the run
 *            goes on. The commands travel on the @c ctrl Docker network, which is NEVER captured:
 *            a command on the @c bus would show up in the capture, be classified by the sensor
 *            and contaminate the measurement. platform/scenario-live checks that no control frame
 *            reaches the bus capture.
 *
 * @details   The protocol is one line of text per datagram, answered with one line:
 *            - @c on / @c off           resume or withdraw the attack (offers, publishing, StopOffer);
 *            - @c status                current state;
 *            - @c set @c period_ms N     publishing period;
 *            - @c set @c so_period_ms N  period of the periodic StopOffer;
 *            - @c set @c so_target A.B.C.D  victim of the StopOffer.
 *            Parsing is a pure function, tested without a socket (tests/test_ctrl_channel.cpp).
 */
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

namespace ids
{
namespace nodes
{

/**
 * @brief One parsed control command.
 */
struct CtrlCommand
{
    /// What the command asks for.
    enum class Kind
    {
        Invalid,      ///< Not understood; @c problem says why.
        On,           ///< Resume the attack.
        Off,          ///< Withdraw the attack.
        Status,       ///< Report the state.
        PeriodMs,     ///< Publishing period, in @c number.
        SoPeriodMs,   ///< StopOffer period, in @c number.
        SoTarget      ///< StopOffer victim, in @c address (host byte order).
    };
    Kind          kind    = Kind::Invalid; ///< The command.
    std::uint32_t number  = 0U;            ///< Numeric argument.
    std::uint32_t address = 0U;            ///< Address argument, host byte order.
    std::string   problem;                 ///< Why it was not understood.
};

/**
 * @brief Parse one command line.
 * @param[in] line Text received, with or without a trailing newline.
 * @return The command; @c Kind::Invalid with a reason when it is not one.
 */
inline CtrlCommand ParseCtrl(const std::string &line)
{
    CtrlCommand        c;
    std::istringstream in(line);
    std::string        verbo;
    std::string        chave;
    std::string        valor;
    in >> verbo >> chave >> valor;
    if ((verbo == "on") && chave.empty())
    {
        c.kind = CtrlCommand::Kind::On;
    }
    else if ((verbo == "off") && chave.empty())
    {
        c.kind = CtrlCommand::Kind::Off;
    }
    else if ((verbo == "status") && chave.empty())
    {
        c.kind = CtrlCommand::Kind::Status;
    }
    else if ((verbo == "set") && ((chave == "period_ms") || (chave == "so_period_ms")))
    {
        char               *fim = nullptr;
        const unsigned long v   = std::strtoul(valor.c_str(), &fim, 10);
        if (valor.empty() || (fim == nullptr) || (*fim != '\0') || (v == 0UL) || (v > 3600000UL))
        {
            c.problem = "periodo invalido: " + valor;
        }
        else
        {
            c.kind   = (chave == "period_ms") ? CtrlCommand::Kind::PeriodMs : CtrlCommand::Kind::SoPeriodMs;
            c.number = static_cast<std::uint32_t>(v);
        }
    }
    else if ((verbo == "set") && (chave == "so_target"))
    {
        struct in_addr a;
        if (::inet_pton(AF_INET, valor.c_str(), &a) == 1)
        {
            c.kind    = CtrlCommand::Kind::SoTarget;
            c.address = ntohl(a.s_addr);
        }
        else
        {
            c.problem = "endereco invalido: " + valor;
        }
    }
    else
    {
        c.problem = "comando desconhecido: " + line;
    }
    return c;
}

/**
 * @brief Non-blocking UDP listener of control commands.
 */
class CtrlChannel final
{
  public:
    CtrlChannel() = default;

    /**
     * @brief Close the socket.
     * @return None.
     */
    ~CtrlChannel()
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

    CtrlChannel(const CtrlChannel &)            = delete; ///< Owns a descriptor.
    CtrlChannel &operator=(const CtrlChannel &) = delete; ///< Owns a descriptor.

    /**
     * @brief Bind to @p address : @p port on the control network.
     * @param[in] address Dotted address of this node on the @c ctrl network.
     * @param[in] port    UDP port.
     * @return Empty on success; otherwise what went wrong.
     */
    std::string Open(const std::string &address, std::uint16_t port)
    {
        struct sockaddr_in local;
        std::memset(&local, 0, sizeof(local));
        local.sin_family = AF_INET;
        local.sin_port   = htons(port);
        if (::inet_pton(AF_INET, address.c_str(), &local.sin_addr) != 1)
        {
            return "endereco de controle invalido: " + address;
        }
        else
        {
            /* Address parsed. */
        }
        m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_fd < 0)
        {
            return std::string("socket de controle: ") + std::strerror(errno);
        }
        else if (::bind(m_fd, reinterpret_cast<const struct sockaddr *>(&local), sizeof(local)) < 0)
        {
            return std::string("bind de controle: ") + std::strerror(errno);
        }
        else
        {
            return std::string();
        }
    }

    /**
     * @brief Take one pending command, without waiting.
     * @param[out] line Text received.
     * @return Whether a command was waiting.
     */
    bool Poll(std::string &line)
    {
        char      buf[256];
        socklen_t n = sizeof(m_peer);
        const ssize_t r = ::recvfrom(m_fd, buf, sizeof(buf) - 1U, MSG_DONTWAIT, reinterpret_cast<struct sockaddr *>(&m_peer), &n);
        if (r <= 0)
        {
            return false;
        }
        else
        {
            buf[r] = '\0';
            line   = std::string(buf);
            while (!line.empty() && ((line.back() == '\n') || (line.back() == '\r')))
            {
                line.pop_back();
            }
            return true;
        }
    }

    /**
     * @brief Answer the sender of the last command.
     * @param[in] text One line.
     * @return None.
     */
    void Reply(const std::string &text) const
    {
        static_cast<void>(::sendto(m_fd, text.data(), text.size(), 0, reinterpret_cast<const struct sockaddr *>(&m_peer), sizeof(m_peer)));
    }

    /**
     * @brief Whether the channel is open.
     * @return True after a successful Open().
     */
    bool IsOpen() const noexcept
    {
        return m_fd >= 0;
    }

  private:
    int                m_fd = -1; ///< Socket, or -1.
    struct sockaddr_in m_peer{};  ///< Sender of the last command.
};

} // namespace nodes
} // namespace ids
