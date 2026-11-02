/**
 * @file      PacketSource.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     The two packet sources, both built on libpcap: file replay and live capture.
 * @version   1.0
 * @date      2026-11-01
 * @copyright Copyright (c) 2026
 *
 * @details   The Python reference reads its captures through @c scapy.utils.RawPcapReader, a reader
 *            of the same file format. Three details decide whether the two sides see the same
 *            bytes, and each one is matched deliberately:
 *            - the frame view spans @c caplen, the CAPTURED length, never @c len, the length the
 *              frame had on the wire. RawPcapReader also yields only the captured bytes, so a
 *              capture truncated by a snapshot length truncates identically on both sides. Using
 *              @c len would additionally read past the buffer libpcap owns;
 *            - the timestamp is @c tv_sec plus @c tv_usec times 1e-6, which is what @c _ts() of
 *              @c src/extract.py computes from the same two header fields;
 *            - the padding the Ethernet layer adds to short frames is part of @c caplen, and
 *              therefore part of the frame handed on. ::ids::SomeipParser depends on that; see the
 *              padding note in its own file.
 *
 * @details   One divergence is worth stating, because it favours this side. On a capture written
 *            with nanosecond timestamps libpcap converts to microseconds by default, whereas scapy
 *            stores the nanoseconds in the field named @c usec and the reference then scales them
 *            as microseconds, landing a thousandfold off. The dataset in use is microsecond
 *            resolution (file magic @c d4c3b2a1), so the difference never materialises there, and
 *            no attempt is made to reproduce the mistake.
 *
 * @details   A capture whose link type is not Ethernet is REJECTED here. The reference does not
 *            look at the link type and would decode such a file against the wrong offsets,
 *            silently. Rejecting can only differ on a file whose features were meaningless anyway.
 */
#include "src/ids_sensor/PacketSource.hpp"

#include <pcap/pcap.h>

#include <cerrno>
#include <cstdio>
#include <utility>

namespace ids
{

namespace
{

constexpr int kLiveSnapshotBytes = 65535; ///< Snapshot length for live capture: the whole frame.
constexpr int kLiveTimeoutMs     = 100;   ///< How long a live read waits before reporting nothing.
constexpr int kEnabled           = 1;     ///< Value that turns a libpcap boolean option on.
constexpr int kFrameDelivered    = 1;     ///< pcap_next_ex() result meaning a frame was handed over.
constexpr int kReadFailed        = -1;    ///< pcap_next_ex() result meaning the read itself failed.

/**
 * @brief Work out WHY a path that libpcap refused could not be used.
 * @param[in] path Path libpcap was given.
 * @return The error code to report.
 * @retval ::ids::IdsSensorErrc::ArtefactNotFound   Nothing exists at @p path.
 * @retval ::ids::IdsSensorErrc::ArtefactUnreadable It exists but cannot be opened for reading; the
 *                                                  support data carries the platform errno.
 * @retval ::ids::IdsSensorErrc::ArtefactCorrupted  It opens for reading, so libpcap refused it over
 *                                                  its contents and not over its accessibility.
 * @details libpcap reports failures as a text buffer and promises nothing about errno, so the
 *          distinction is drawn by opening the path separately. That mirrors what
 *          ::ids::ByteModels::Create does, and keeps "not deployed" apart from "deployed and
 *          unreadable", which are different problems for whoever has to fix the deployment.
 */
ara::core::ErrorCode ClassifyOpenFailure(const std::string &path)
{
    errno                  = 0;
    std::FILE *const probe = std::fopen(path.c_str(), "rb");
    if (probe == nullptr)
    {
        const int reason = errno;
        if (reason == ENOENT)
        {
            return ara::core::ErrorCode(IdsSensorErrc::ArtefactNotFound);
        }
        return ara::core::ErrorCode(IdsSensorErrc::ArtefactUnreadable, reason);
    }
    else
    {
        /* Readable, so the refusal was about the contents. */
    }
    static_cast<void>(std::fclose(probe));
    return ara::core::ErrorCode(IdsSensorErrc::ArtefactCorrupted);
}

/**
 * @brief Turn a libpcap header and buffer into a frame.
 * @param[in] header Record header libpcap filled in.
 * @param[in] bytes  Buffer libpcap owns, holding @c caplen bytes.
 * @return The frame, as a view over that buffer.
 * @warning The view is valid only until the next read on the same handle.
 */
Frame MakeFrame(const pcap_pkthdr &header, const std::uint8_t *bytes)
{
    Frame frame;
    /* caplen, not len: see the note at the top of this file. */
    frame.bytes = PayloadView(bytes, static_cast<std::size_t>(header.caplen));
    frame.ts    = (static_cast<double>(header.ts.tv_sec) + (static_cast<double>(header.ts.tv_usec) * 1e-6));
    return frame;
}

} // namespace

/**
 * @brief Reader state of a PCAP replay.
 */
struct PcapSource::Impl
{
    pcap_t *handle      = nullptr; ///< Open reader; owned, closed by the destructor.
    bool    streamBroke = false;   ///< Whether replay stopped at a read error instead of at the end.

    /**
     * @brief Construct empty, before a reader has been taken over.
     * @return None.
     */
    Impl() = default;

    /**
     * @brief Close the reader.
     * @return None.
     */
    ~Impl()
    {
        if (handle != nullptr)
        {
            pcap_close(handle);
            handle = nullptr;
        }
        else
        {
            /* Nothing to close. */
        }
    }

    Impl(const Impl &) = delete;            ///< Owns a handle, so it is not copyable.
    Impl &operator=(const Impl &) = delete; ///< Owns a handle, so it is not assignable.
};

namespace
{

/**
 * @brief Compile and install a BPF filter on an already opened handle.
 * @param[in,out] handle Opened pcap handle.
 * @param[in]     filter BPF expression; an empty one installs nothing and succeeds.
 * @return Nothing on success, or an error.
 * @retval ::ids::IdsSensorErrc::InvalidConfiguration The expression did not compile, or could not
 *                                                   be installed on the handle.
 * @details The netmask is PCAP_NETMASK_UNKNOWN on purpose. It is only consulted for the @c broadcast
 *          primitive, which none of the bench filters use, and asking the interface for it would
 *          make an offline handle fail for no reason.
 */
ara::core::Result<void> InstalaFiltro(pcap_t *handle, const std::string &filter)
{
    using Res = ara::core::Result<void>;

    if (filter.empty())
    {
        return Res::FromValue();
    }
    else
    {
        /* Something to install. */
    }

    bpf_program programa;
    if (pcap_compile(handle, &programa, filter.c_str(), kEnabled, PCAP_NETMASK_UNKNOWN) != 0)
    {
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::InvalidConfiguration));
    }
    else
    {
        /* Compiled. */
    }

    const int instalado = pcap_setfilter(handle, &programa);
    /* The program is freed either way: pcap_setfilter copies what it needs. */
    pcap_freecode(&programa);
    if (instalado != 0)
    {
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::InvalidConfiguration));
    }
    else
    {
        /* Installed. */
    }
    return Res::FromValue();
}

} // namespace

ara::core::Result<std::unique_ptr<PcapSource>> PcapSource::Create(const std::string &path, const std::string &filter)
{
    using Res = ara::core::Result<std::unique_ptr<PcapSource>>;

    char          errbuf[PCAP_ERRBUF_SIZE] = {0};
    pcap_t *const handle                   = pcap_open_offline(path.c_str(), errbuf);
    if (handle == nullptr)
    {
        return Res::FromError(ClassifyOpenFailure(path));
    }
    else
    {
        /* The file opened. */
    }

    /* The Impl owns the handle from here on, so every return path below closes it. */
    std::unique_ptr<Impl> impl(new Impl());
    impl->handle = handle;

    if (pcap_datalink(handle) != DLT_EN10MB)
    {
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::ArtefactCorrupted));
    }
    else
    {
        /* Ethernet, which is what ::ids::SomeipParser decodes. */
    }

    const ara::core::Result<void> filtrado = InstalaFiltro(handle, filter);
    if (!filtrado.HasValue())
    {
        return Res::FromError(filtrado.Error());
    }
    else
    {
        /* No filter, or one that compiled and was installed. */
    }

    return Res::FromValue(std::unique_ptr<PcapSource>(new PcapSource(std::move(impl))));
}

PcapSource::PcapSource(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl))
{
}

PcapSource::~PcapSource() = default;

ara::core::Optional<Frame> PcapSource::Next()
{
    ara::core::Optional<Frame> frame;

    pcap_pkthdr        *header = nullptr;
    const std::uint8_t *bytes  = nullptr;
    const int           status = pcap_next_ex(m_impl->handle, &header, &bytes);
    if (status == kFrameDelivered)
    {
        frame = ara::core::Optional<Frame>(MakeFrame(*header, bytes));
    }
    else if (status == kReadFailed)
    {
        /* A read error partway through the file. It leaves the Optional empty, which
         * is also what the end of the file does, so the fact that the run was cut
         * short is recorded for StreamStatus() to report. Without that, a truncated
         * replay would be indistinguishable from a complete one, and a parity figure
         * over a dataset would quietly cover fewer packets than it claims. */
        m_impl->streamBroke = true;
    }
    else
    {
        /* PCAP_ERROR_BREAK, the end of the file: the one normal way a replay stops. */
    }

    return frame;
}

ara::core::Result<void> PcapSource::StreamStatus() const
{
    using Res = ara::core::Result<void>;

    if (m_impl->streamBroke)
    {
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::ArtefactCorrupted));
    }
    else
    {
        /* Every record in the file was delivered. */
    }
    return Res::FromValue();
}

/**
 * @brief Capture state of a live source.
 */
struct LiveSource::Impl
{
    pcap_t *handle = nullptr; ///< Active capture; owned, closed by the destructor.

    /**
     * @brief Construct empty, before a capture has been taken over.
     * @return None.
     */
    Impl() = default;

    /**
     * @brief Stop capturing and release the handle.
     * @return None.
     */
    ~Impl()
    {
        if (handle != nullptr)
        {
            pcap_close(handle);
            handle = nullptr;
        }
        else
        {
            /* Nothing to close. */
        }
    }

    Impl(const Impl &) = delete;            ///< Owns a handle, so it is not copyable.
    Impl &operator=(const Impl &) = delete; ///< Owns a handle, so it is not assignable.
};

ara::core::Result<std::unique_ptr<LiveSource>> LiveSource::Create(const std::string &iface, const std::string &filter)
{
    using Res = ara::core::Result<std::unique_ptr<LiveSource>>;

    /* pcap_create plus pcap_activate, rather than pcap_open_live, because only this
     * pair distinguishes "no such interface" from "not allowed to capture on it", and
     * those are the two failures a deployment actually runs into. */
    char          errbuf[PCAP_ERRBUF_SIZE] = {0};
    pcap_t *const handle                   = pcap_create(iface.c_str(), errbuf);
    if (handle == nullptr)
    {
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::ArtefactUnreadable));
    }
    else
    {
        /* The handle exists but is not capturing yet. */
    }

    /* The Impl owns the handle from here on, so every return path below closes it. */
    std::unique_ptr<Impl> impl(new Impl());
    impl->handle = handle;

    static_cast<void>(pcap_set_snaplen(handle, kLiveSnapshotBytes));
    static_cast<void>(pcap_set_promisc(handle, kEnabled));
    static_cast<void>(pcap_set_timeout(handle, kLiveTimeoutMs));
    /* Immediate mode: a frame is handed over as it arrives, instead of waiting for a
     * kernel buffer to fill. A sensor that batches is a sensor that reports late. */
    static_cast<void>(pcap_set_immediate_mode(handle, kEnabled));

    const int activated = pcap_activate(handle);
    if (activated == PCAP_ERROR_NO_SUCH_DEVICE)
    {
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::ArtefactNotFound));
    }
    else if (activated == PCAP_ERROR_PERM_DENIED)
    {
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::ArtefactUnreadable, EACCES));
    }
    else if (activated < 0)
    {
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::ArtefactUnreadable));
    }
    else
    {
        /* Zero, or a positive warning such as promiscuous mode being unavailable. A
         * warning still leaves a usable capture, so it is not treated as a failure. */
    }

    if (pcap_datalink(handle) != DLT_EN10MB)
    {
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::ArtefactCorrupted));
    }
    else
    {
        /* Ethernet, which is what ::ids::SomeipParser decodes. */
    }

    /* O filtro entra DEPOIS do activate, que e quando o handle tem link-layer e o
     * pcap_compile pode conferir a expressao contra ele. Antes disso uma expressao
     * valida para Ethernet seria recusada. */
    const ara::core::Result<void> filtrado = InstalaFiltro(handle, filter);
    if (!filtrado.HasValue())
    {
        return Res::FromError(filtrado.Error());
    }
    else
    {
        /* No filter, or one that compiled and was installed. */
    }

    return Res::FromValue(std::unique_ptr<LiveSource>(new LiveSource(std::move(impl))));
}

LiveSource::LiveSource(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl))
{
}

LiveSource::~LiveSource() = default;

ara::core::Optional<Frame> LiveSource::Next()
{
    ara::core::Optional<Frame> frame;

    pcap_pkthdr        *header = nullptr;
    const std::uint8_t *bytes  = nullptr;
    const int           status = pcap_next_ex(m_impl->handle, &header, &bytes);
    if (status == kFrameDelivered)
    {
        frame = ara::core::Optional<Frame>(MakeFrame(*header, bytes));
    }
    else
    {
        /* Zero means the timeout elapsed with nothing to show, which is the usual case
         * on a quiet bus. An empty Optional therefore means "no frame right now" here,
         * NOT "the stream has ended" as it does for a replay; a live capture has no
         * end. The caller loops, and the timeout is what gives it the chance to notice
         * its own shutdown request between frames. */
    }

    return frame;
}

} // namespace ids
