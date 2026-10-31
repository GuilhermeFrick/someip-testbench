/**
 * @file      FeatureStreamer.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Extraction of the 16 features, packet by packet, with sliding-window state.
 * @version   3.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   Concrete, with no interface: there is a single implementation, and it is precisely
 *            what the parity test pins down.
 *
 *            This is the only STATEFUL stage, and therefore the riskiest one. A window that starts
 *            full instead of empty, or an update order different from the Python side, is enough
 *            to make the features diverge silently. Three asymmetries of the reference are easy to
 *            miss and are reproduced deliberately, each marked in the source:
 *            - the payload-repetition rate is computed BEFORE the current payload joins its
 *              window, so the window holds only earlier packets;
 *            - the source-diversity ratio is computed AFTER the current payload joins its window,
 *              so that one DOES include the current packet;
 *            - the repetition window takes every payload, empty ones included, while the diversity
 *              window takes only non-empty ones.
 *
 * @details   Acceptance criterion: the same PCAP produces the same 16 features in Python and here.
 *            See tests/README.md.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "src/ara/core/Optional.hpp"
#include "src/ara/core/Result.hpp"
#include "src/ids_sensor/ByteModel.hpp"
#include "src/ids_sensor/IdsSensorErrorDomain.hpp"
#include "src/ids_sensor/types.hpp"

/** @addtogroup IdsSensorFeatureStreamer Feature extractor
 *  @ingroup IdsSensor
 *  @{
 */

namespace ids
{

/**
 * @brief Window parameters and memory caps of the extractor.
 * @warning The two window sizes must match the values used during training, otherwise the features
 *          change meaning while still looking plausible.
 */
struct StreamerConfig
{
    std::size_t recentPayloadWindow = 5U;    ///< Payload repetition window, per flow. Contract with training.
    std::size_t sourcePayloadWindow = 1000U; ///< Payload diversity window, per source. Contract with training.
    std::size_t maxFlows            = 4096U; ///< Cap on flows kept in memory; the oldest is evicted beyond it.
    std::size_t maxSources          = 256U;  ///< Cap on sources kept in memory; the oldest is evicted beyond it.

    /// Payload bytes reserved per flow for the Hamming comparison against the previous packet.
    /// A payload beyond this grows the buffer once; it is a reservation, not a limit, so nothing
    /// is truncated and the feature stays exact.
    std::size_t reservedPayloadBytes = 64U;
};

/**
 * @brief Produces the 16 raw features from a parsed packet.
 */
class FeatureStreamer final
{
  public:
    /**
     * @brief Build an extractor bound to a set of byte models.
     * @param[in] models Byte models used for log-likelihood and cross entropy.
     * @param[in] cfg    Window parameters and caps.
     * @return The extractor, or an error when the configuration is unusable.
     * @retval ::ids::IdsSensorErrc::InvalidConfiguration A window size or a capacity is zero.
     * @details Returns a smart pointer, per section 8.2 of the C++ code standard. The containers
     *          are reserved here, so Step() does not allocate for traffic within the reservation.
     * @warning @p models must outlive the extractor.
     */
    static ara::core::Result<std::unique_ptr<FeatureStreamer>> Create(const ByteModels &models, const StreamerConfig &cfg);

    /**
     * @brief Release the window state.
     * @return None.
     */
    ~FeatureStreamer();

    /**
     * @brief Consume one packet and produce the 16 RAW features, not normalised.
     * @param[in] packet Parsed packet to consume.
     * @return The 16 raw features, or an empty Optional when the packet produces no vector.
     * @details Absence is a normal outcome here, not a failure, which is why this is an Optional
     *          and not a Result.
     * @warning Consuming a packet UPDATES the window state, so calling this twice with the same
     *          packet counts it twice and the second vector differs from the first.
     */
    ara::core::Optional<FeatureVec> Step(const ParsedPacket &packet);

    /**
     * @brief Clear all window state.
     * @return None.
     * @details Used between runs so that measurement is repeatable, and between parity cases so
     *          one case does not leak state into the next. Also zeroes the eviction counters.
     */
    void Reset();

    /**
     * @brief How many flows were evicted because ::ids::StreamerConfig::maxFlows was reached.
     * @return Number of evictions since construction or the last Reset().
     * @details THE PARITY TEST MUST ASSERT THIS IS ZERO. The Python reference keeps per-flow state
     *          forever, with no cap. The cap here exists because a sensor running for hours would
     *          otherwise leak, but the moment it evicts, this extractor and the reference stop
     *          computing the same thing. Without this counter a divergence caused by eviction
     *          would look like an arithmetic bug.
     */
    std::uint64_t FlowEvictions() const noexcept;

    /**
     * @brief How many sources were evicted because ::ids::StreamerConfig::maxSources was reached.
     * @return Number of evictions since construction or the last Reset().
     * @details Same warning as FlowEvictions().
     */
    std::uint64_t SourceEvictions() const noexcept;

    /**
     * @brief Copy construction is deleted: the extractor owns window state.
     * @return None.
     */
    FeatureStreamer(const FeatureStreamer &) = delete;

    /**
     * @brief Copy assignment is deleted: the extractor owns window state.
     * @return Nothing; the operator cannot be called.
     */
    FeatureStreamer &operator=(const FeatureStreamer &) = delete;

  private:
    struct Impl; ///< Window containers, kept out of this header.

    /**
     * @brief Construct from already reserved window state.
     * @param[in,out] impl Reserved state, taken over by this extractor.
     * @return None.
     * @details Private so that Create() is the only way in.
     */
    explicit FeatureStreamer(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl; ///< Owned window state.
};

} // namespace ids

/** @}*/ // End of IdsSensorFeatureStreamer
