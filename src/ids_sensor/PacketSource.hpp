/**
 * @file      PacketSource.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Packet source interface and its two implementations: PCAP replay and live capture.
 * @version   2.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   In practice this is the most useful of the three interfaces in the component: it is
 *            what makes the SAME sensor run offline over a PCAP and online on the bus. Without it
 *            the parity test would need a code path separate from the production one, and would
 *            then stop testing what actually runs.
 *
 * @details   Next() returns ara::core::Optional rather than a bool plus an output parameter, per
 *            rule A8-4-8. End of stream is a normal outcome, not a failure, which is why it is an
 *            Optional and not a Result.
 */
#pragma once

#include <memory>
#include <string>

#include "src/ara/core/Optional.hpp"
#include "src/ara/core/Result.hpp"
#include "src/ids_sensor/IdsSensorErrorDomain.hpp"
#include "src/ids_sensor/types.hpp"

/** @addtogroup IdsSensorPacketSource Packet sources
 *  @ingroup IdsSensor
 *  @{
 */

namespace ids
{

/**
 * @brief Abstract source of frames.
 */
class IPacketSource
{
  public:
    /**
     * @brief Virtual destructor.
     * @return None.
     */
    virtual ~IPacketSource() = default;

    /**
     * @brief Deliver the next frame.
     * @return The next frame, or an empty Optional at the end of the stream.
     * @warning The view inside the returned frame points into a buffer the source owns, and is
     *          only valid until the next call.
     */
    virtual ara::core::Optional<Frame> Next() = 0;
};

/**
 * @brief PCAP replay: deterministic and repeatable, for parity and measurement.
 */
class PcapSource final : public IPacketSource
{
  public:
    /**
     * @brief Open a capture file for replay.
     * @param[in] path Path to the PCAP file.
     * @return The opened source, or an error describing why it could not be opened.
     * @retval ::ids::IdsSensorErrc::ArtefactNotFound   No file exists at @p path.
     * @retval ::ids::IdsSensorErrc::ArtefactUnreadable The file exists but could not be opened;
     *                                                 the support data carries the platform errno.
     * @retval ::ids::IdsSensorErrc::ArtefactCorrupted  The file is not a capture this build can read.
     * @details Returns a smart pointer, per section 8.2 of the C++ code standard.
     */
    static ara::core::Result<std::unique_ptr<PcapSource>> Create(const std::string &path);

    /**
     * @brief Close the capture file and release the reader.
     * @return None.
     */
    ~PcapSource() override;

    /**
     * @copydoc ::ids::IPacketSource::Next()
     */
    ara::core::Optional<Frame> Next() override;

    /**
     * @brief Say whether the replay consumed the whole file or stopped at a read error.
     * @return An empty value when the file was read to its end, or the error that cut it short.
     * @retval ::ids::IdsSensorErrc::ArtefactCorrupted A read failed partway through the file.
     * @details Next() signals both the end of the file and a read error the same way, as an empty
     *          Optional, because ending is not a failure and the interface therefore returns no
     *          Result. That makes a truncated replay look exactly like a complete one, which would
     *          let a figure measured over a dataset quietly cover fewer packets than it claims.
     *          Whoever counts packets calls this once the loop ends.
     */
    ara::core::Result<void> StreamStatus() const;

  private:
    struct Impl; ///< Reader state, kept out of this header.

    /**
     * @brief Construct from an already opened reader.
     * @param[in,out] impl Opened reader state, taken over by this source.
     * @return None.
     * @details Private so that Create() is the only way in.
     */
    explicit PcapSource(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl; ///< Owned reader state.
};

/**
 * @brief Live capture on the bus interface.
 */
class LiveSource final : public IPacketSource
{
  public:
    /**
     * @brief Start capturing on an interface.
     * @param[in] iface Name of the interface to capture from.
     * @return The started source, or an error describing why capture could not start.
     * @retval ::ids::IdsSensorErrc::ArtefactNotFound   No such interface.
     * @retval ::ids::IdsSensorErrc::ArtefactUnreadable Capture could not be started, typically for
     *                                                 want of privileges; the support data carries
     *                                                 the platform errno.
     * @warning The capturing process must have joined the relevant IGMP groups, otherwise the
     *          Docker bridge does not deliver the Service Discovery multicast, and this source
     *          returns an empty Optional on the first call with no error to show for it. See
     *          platform/sd-smoke/README.md.
     */
    static ara::core::Result<std::unique_ptr<LiveSource>> Create(const std::string &iface);

    /**
     * @brief Stop capturing and release the handle.
     * @return None.
     */
    ~LiveSource() override;

    /**
     * @copydoc ::ids::IPacketSource::Next()
     */
    ara::core::Optional<Frame> Next() override;

  private:
    struct Impl; ///< Capture state, kept out of this header.

    /**
     * @brief Construct from an already started capture.
     * @param[in,out] impl Started capture state, taken over by this source.
     * @return None.
     * @details Private so that Create() is the only way in.
     */
    explicit LiveSource(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl; ///< Owned capture state.
};

} // namespace ids

/** @}*/ // End of IdsSensorPacketSource
