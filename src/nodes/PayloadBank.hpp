/**
 * @file      PayloadBank.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Payload bytes taken from a reference capture, replayed in order by event.
 * @version   1.0
 * @date      2026-11-02
 * @copyright Copyright (c) 2026
 *
 * @details   The bench measured that payload CONTENT dominates the false positive: the two
 *            synthetic policies of the generic node are bad for OPPOSITE reasons -- @c counter is
 *            nearly deterministic and @c random is uniform. Neither answers how much of the gap is
 *            content alone. Feeding the nodes the bytes of the reference bus itself answers it: if
 *            the false positive falls to the level of the dataset, content is the whole gap and the
 *            rest of the bench is right.
 *
 * @warning   This is CIRCULAR as a product claim and must never be presented as "the bench
 *            reproduces the bus". It is the opposite: an UPPER BOUND on what the reproduction can
 *            reach, and a way to decide whether realistic content is worth chasing.
 *
 * @details   THE ORDER MATTERS. Four of the sixteen features compare a packet with the PREVIOUS one
 *            of the same flow (@c someip_change, @c sd_change, @c l4_change, @c repeat_rate), so the
 *            bank hands samples out in the order they were captured and wraps around at the end.
 *            Drawing at random would destroy that structure and produce a third kind of synthetic
 *            traffic rather than the dataset's.
 *
 * @details   File format, little endian, written by tools/extract_payloads.py:
 *            @code
 *            magic   4 bytes "SPAY"
 *            version u32 = 1
 *            streams u32
 *            per stream: service u16, event u16, size u32, samples u32,
 *                        then samples * size bytes
 *            @endcode
 *            Every stream is fixed size because that is what the dataset has: over the benign
 *            capture each of the eleven notification streams carries exactly one payload size, and
 *            those sizes match the @c payload_bytes the matrix recovered.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "src/libecu/Types.hpp"

/** @addtogroup NodesPayloadBank PayloadBank
 *  @ingroup Nodes
 *  @{
 */

namespace ids
{
namespace nodes
{

/**
 * @brief Payload samples of one event, handed out in capture order.
 */
class PayloadStream
{
  public:
    /**
     * @brief Build a stream over already read bytes.
     * @param[in] size    Size of every sample, in bytes.
     * @param[in] samples Concatenated samples; its size must be a multiple of @p size.
     * @return None.
     */
    PayloadStream(std::size_t size, std::vector<std::uint8_t> samples)
        : m_size(size), m_bytes(std::move(samples)), m_next(0U)
    {
    }

    /// Size of every sample of this stream.
    std::size_t SampleSize() const
    {
        return m_size;
    }

    /// How many samples the stream holds.
    std::size_t Count() const
    {
        return (m_size == 0U) ? 0U : (m_bytes.size() / m_size);
    }

    /**
     * @brief The next sample, advancing the cursor.
     * @return View over the sample; empty when the stream holds none.
     * @details Wraps around at the end. A run longer than the captured stretch therefore repeats
     *          it, which keeps the local order intact and only repeats the sequence as a whole.
     */
    ::ids::ecu::ByteView Next()
    {
        const std::size_t total = Count();
        if (total == 0U)
        {
            return ::ids::ecu::ByteView();
        }
        else
        {
            /* There is at least one sample. */
        }
        const std::size_t offset = m_next * m_size;
        m_next                   = (m_next + 1U) % total;
        return ::ids::ecu::ByteView(&m_bytes[offset], m_size);
    }

  private:
    std::size_t               m_size;  ///< Bytes per sample.
    std::vector<std::uint8_t> m_bytes; ///< Samples, back to back.
    std::size_t               m_next;  ///< Cursor into the samples.
};

/**
 * @brief Every payload stream of a capture, by service and event.
 */
class PayloadBank
{
  public:
    /**
     * @brief Read a bank from disk.
     * @param[in] path File written by tools/extract_payloads.py.
     * @return The bank. Its Problem() is empty when the file was usable.
     * @details Reports the fault in the object rather than throwing, like ::ids::nodes::NodePlan:
     *          the node prints the sentence and exits, and a bank that could not be read must never
     *          silently become an empty one.
     */
    static PayloadBank FromFile(const std::string &path)
    {
        PayloadBank banco;

        std::FILE *const fh = std::fopen(path.c_str(), "rb");
        if (fh == nullptr)
        {
            banco.m_problem = "nao abriu " + path;
            return banco;
        }
        else
        {
            /* Open. */
        }

        char          magic[4] = {0, 0, 0, 0};
        std::uint32_t versao   = 0U;
        std::uint32_t fluxos   = 0U;
        bool          ok       = (std::fread(magic, 1U, 4U, fh) == 4U);
        ok                     = ok && (std::fread(&versao, sizeof(versao), 1U, fh) == 1U);
        ok                     = ok && (std::fread(&fluxos, sizeof(fluxos), 1U, fh) == 1U);

        if (!ok || (magic[0] != 'S') || (magic[1] != 'P') || (magic[2] != 'A') || (magic[3] != 'Y'))
        {
            banco.m_problem = path + " nao e um banco de payloads (magic errado ou truncado)";
            static_cast<void>(std::fclose(fh));
            return banco;
        }
        else if (versao != 1U)
        {
            banco.m_problem = path + ": versao de formato nao suportada";
            static_cast<void>(std::fclose(fh));
            return banco;
        }
        else
        {
            /* Header is usable. */
        }

        for (std::uint32_t i = 0U; (i < fluxos) && banco.m_problem.empty(); ++i)
        {
            std::uint16_t servico  = 0U;
            std::uint16_t evento   = 0U;
            std::uint32_t tamanho  = 0U;
            std::uint32_t amostras = 0U;

            bool lido = (std::fread(&servico, sizeof(servico), 1U, fh) == 1U);
            lido      = lido && (std::fread(&evento, sizeof(evento), 1U, fh) == 1U);
            lido      = lido && (std::fread(&tamanho, sizeof(tamanho), 1U, fh) == 1U);
            lido      = lido && (std::fread(&amostras, sizeof(amostras), 1U, fh) == 1U);
            if (!lido)
            {
                banco.m_problem = path + ": cabecalho de fluxo truncado";
                break;
            }
            else if ((tamanho == 0U) || (tamanho > ::ids::ecu::kMaxPayloadBytes))
            {
                banco.m_problem = path + ": tamanho de amostra fora do admissivel";
                break;
            }
            else
            {
                /* Stream header is usable. */
            }

            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(tamanho) * amostras);
            if ((!bytes.empty()) && (std::fread(bytes.data(), 1U, bytes.size(), fh) != bytes.size()))
            {
                banco.m_problem = path + ": amostras truncadas";
                break;
            }
            else
            {
                /* Samples read. */
            }

            banco.m_fluxos.insert(std::make_pair(Chave(servico, evento), PayloadStream(tamanho, std::move(bytes))));
        }

        static_cast<void>(std::fclose(fh));
        return banco;
    }

    /// Empty when the bank is usable; otherwise what is wrong with the file.
    const std::string &Problem() const
    {
        return m_problem;
    }

    /// How many streams the bank holds.
    std::size_t Count() const
    {
        return m_fluxos.size();
    }

    /**
     * @brief The stream of one event.
     * @param[in] service  Service identifier.
     * @param[in] notifier Event identifier.
     * @return Pointer to the stream, or null when the bank has none for that event.
     */
    PayloadStream *Find(::ids::ecu::ServiceId service, ::ids::ecu::EventId notifier)
    {
        const std::map<std::uint32_t, PayloadStream>::iterator it = m_fluxos.find(Chave(service, notifier));
        return (it == m_fluxos.end()) ? nullptr : &it->second;
    }

  private:
    /// Service and event packed into one key, so the map needs no custom comparator.
    static std::uint32_t Chave(std::uint16_t service, std::uint16_t notifier)
    {
        return (static_cast<std::uint32_t>(service) << 16U) | notifier;
    }

    std::map<std::uint32_t, PayloadStream> m_fluxos;  ///< Streams, by packed key.
    std::string                            m_problem; ///< Empty when the bank is usable.
};

} // namespace nodes
} // namespace ids

/** @}*/ // End of NodesPayloadBank
