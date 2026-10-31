/**
 * @file      PacketSource.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Packet source interface and its two implementations: PCAP replay and live capture.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   In practice this is the most useful of the three interfaces in the component: it is
 *            what makes the SAME sensor run offline over a PCAP and online on the bus. Without it
 *            the parity test would need a code path separate from the production one, and would
 *            then stop testing what actually runs.
 */
#pragma once

#include <memory>
#include <string>

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
     * @param[out] out Next frame of the stream.
     * @return Whether a frame was delivered.
     * @retval true  @p out holds a valid frame.
     * @retval false End of stream; @p out is unchanged.
     * @warning The buffer referenced by @p out is only valid until the next call.
     */
    virtual bool Next(Frame &out) = 0;
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
     * @return None.
     */
    explicit PcapSource(const std::string &path);

    /**
     * @brief Close the capture file and release the reader.
     * @return None.
     */
    ~PcapSource() override;

    /**
     * @copydoc ::ids::IPacketSource::Next(Frame &)
     */
    bool Next(Frame &out) override;

  private:
    struct Impl;                     ///< Reader state, kept out of this header.
    std::unique_ptr<Impl> m_impl;    ///< Owned reader state.
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
     * @return None.
     * @warning The capturing process must have joined the relevant IGMP groups, otherwise the
     *          Docker bridge does not deliver the Service Discovery multicast. See
     *          platform/sd-smoke/README.md.
     */
    explicit LiveSource(const std::string &iface);

    /**
     * @brief Stop capturing and release the handle.
     * @return None.
     */
    ~LiveSource() override;

    /**
     * @copydoc ::ids::IPacketSource::Next(Frame &)
     */
    bool Next(Frame &out) override;

  private:
    struct Impl;                     ///< Capture state, kept out of this header.
    std::unique_ptr<Impl> m_impl;    ///< Owned capture state.
};

} // namespace ids

/** @}*/ // End of IdsSensorPacketSource
