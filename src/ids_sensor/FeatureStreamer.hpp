/**
 * @file      FeatureStreamer.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Extraction of the 16 features, packet by packet, with sliding-window state.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   Concrete, with no interface: there is a single implementation, and it is precisely
 *            what the parity test pins down.
 *
 *            This is the only STATEFUL stage, and therefore the riskiest one: a window that starts
 *            full instead of empty, or an update order different from the Python side, is enough
 *            to make the features diverge silently.
 *
 *            Acceptance criterion: the same PCAP produces the same 16 features in Python and here.
 */
#pragma once

#include <cstddef>
#include <memory>

#include "src/ids_sensor/ByteModel.hpp"
#include "src/ids_sensor/types.hpp"

/** @addtogroup IdsSensorFeatureStreamer Feature extractor
 *  @ingroup IdsSensor
 *  @{
 */

namespace ids
{

/**
 * @brief Window parameters of the extractor.
 * @warning These must match the values used during training, otherwise the features change
 *          meaning while still looking plausible.
 */
struct StreamerConfig
{
    std::size_t recentPayloadWindow = 5U;    ///< Payload repetition window, per flow.
    std::size_t sourcePayloadWindow = 1000U; ///< Payload diversity window, per source.
    std::size_t maxFlows            = 4096U; ///< Cap on flows kept in memory.
    std::size_t maxSources          = 256U;  ///< Cap on sources kept in memory.
};

/**
 * @brief Produces the 16 raw features from a parsed packet.
 * @details The caps in StreamerConfig exist for the online case only. Offline, over a fixed PCAP,
 *          the per-flow state simply grows and nothing happens. In a sensor running for hours that
 *          is a leak: every new flow adds state that never leaves. With a cap, the oldest entry is
 *          evicted. It is a problem that only exists online, and that offline validation would not
 *          catch.
 */
class FeatureStreamer final
{
  public:
    /**
     * @brief Build an extractor bound to a set of byte models.
     * @param[in] models Byte models used for log-likelihood and cross entropy.
     * @param[in] cfg    Window parameters, which must match training.
     * @return None.
     */
    FeatureStreamer(const ByteModels &models, const StreamerConfig &cfg);

    /**
     * @brief Release the window state.
     * @return None.
     */
    ~FeatureStreamer();

    /**
     * @brief Consume one packet and produce the 16 RAW features, not normalised.
     * @param[in]  packet Parsed packet to consume.
     * @param[out] out    Raw feature vector.
     * @return Whether the packet produced a feature vector.
     * @retval true  @p out holds the 16 raw features.
     * @retval false The packet produces no vector, for instance because it is not SOME/IP.
     * @warning Does not allocate on the hot path.
     */
    bool Step(const ParsedPacket &packet, FeatureVec &out);

    /**
     * @brief Clear all window state.
     * @return None.
     * @details Used between runs so that measurement is repeatable.
     */
    void Reset();

  private:
    struct Impl;                  ///< Window containers, kept out of this header.
    std::unique_ptr<Impl> m_impl; ///< Owned window state.
};

} // namespace ids

/** @}*/ // End of IdsSensorFeatureStreamer
