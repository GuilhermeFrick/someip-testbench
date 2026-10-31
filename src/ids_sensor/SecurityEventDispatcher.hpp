/**
 * @file      SecurityEventDispatcher.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Sensor output: turns a predicted traffic class into a security event reported to the
 *            Intrusion Detection System Manager.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   This file replaces the earlier EventReporter.hpp, which had the shape wrong. It took
 *            the event type as a PARAMETER of the reporting call. In ara::idsm the event type is
 *            fixed per reporter, and comes from the ara::core::InstanceSpecifier given at its
 *            construction, so that the platform resolves the filter chain once instead of on every
 *            call. See ../ara/idsm/EventReporter.hpp.
 *
 *            The consequence is this dispatcher. It owns ONE ara::idsm::EventReporter per
 *            reportable class and selects between them, so the sensor can keep speaking in terms
 *            of the class it predicted while the platform sees a fixed event type per reporter.
 *
 * @details   The interface exists because the destination genuinely varies: on the bench it is the
 *            IdsM through ara::idsm; in the tests it is an in-memory collector. The sensor must not
 *            know which one is active.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "src/ara/core/Result.hpp"
#include "src/ara/idsm/EventReporter.hpp"
#include "src/ids_sensor/IdsSensorErrorDomain.hpp"
#include "src/ids_sensor/types.hpp"

/** @addtogroup IdsSensorEventDispatch Security event dispatch
 *  @ingroup IdsSensor
 *  @{
 */

namespace ids
{

using SecurityEventTimestamp = ara::idsm::TimestampType; ///< Event time, in the unit the platform configures.

/**
 * @brief Typed context of one detection, before it is serialised for the IdsM.
 * @details ara::idsm carries context data as OPAQUE bytes whose layout the meta-model defines. This
 *          struct is the typed form the sensor fills, and Serialize() produces those bytes. Keeping
 *          the two apart is what lets the layout change with the meta-model without touching the
 *          detection path.
 */
struct SecurityEventContext
{
    std::uint32_t srcIp      = 0U;   ///< Source IPv4 address of the offending packet, host byte order.
    std::uint16_t serviceId  = 0U;   ///< SOME/IP service identifier.
    std::uint16_t methodId   = 0U;   ///< SOME/IP method or event identifier.
    float         confidence = 0.0F; ///< Probability of the predicted class.

    static constexpr std::size_t SERIALISED_SIZE = 12U; ///< Size of the serialised form, in bytes.

    using Buffer = std::array<std::uint8_t, SERIALISED_SIZE>; ///< Serialised context bytes.

    /**
     * @brief Serialise this context into the layout the meta-model defines.
     * @return The serialised bytes: source address, service identifier, method identifier and
     *         confidence, each in network byte order.
     * @details Returns the buffer by value, so reporting never allocates and the caller keeps
     *          ownership of the bytes for as long as the reporting call needs them.
     */
    Buffer Serialize() const;
};

/**
 * @brief Abstract security event sink.
 */
class ISecurityEventDispatcher
{
  public:
    /**
     * @brief Virtual destructor.
     * @return None.
     */
    virtual ~ISecurityEventDispatcher() = default;

    /**
     * @brief Report the detection of one class, if that class raises an event.
     * @param[in] trafficClass Class predicted by the classifier.
     * @param[in] context      Typed context of the detection.
     * @param[in] timestamp    Time of the detection, from the single scenario clock.
     * @return Whether an event was raised.
     * @retval true  The class maps to a security event, and it was reported.
     * @retval false The class raises no event, which is the case for ::ids::TrafficClass::Normal.
     * @details Filtering, aggregation and qualification belong to the IdsM. This side only states
     *          what was observed, which is why there is no failure to report back.
     */
    virtual bool Dispatch(TrafficClass trafficClass, const SecurityEventContext &context, SecurityEventTimestamp timestamp) = 0;
};

/**
 * @brief Dispatcher that reports through ara::idsm.
 * @details Holds one ara::idsm::EventReporter per reportable class, each bound at construction to
 *          the security event definition its specifier names.
 */
class IdsmDispatcher final : public ISecurityEventDispatcher
{
  public:
    /**
     * @brief Build the reporters for every reportable class.
     * @param[in] portPrefix Meta-model path prefix under which the security event ports are
     *                       configured, for instance @c "ids_sensor/SecurityEvents". The class
     *                       name is appended to it to form each ara::core::InstanceSpecifier.
     * @return The dispatcher, or an error when some event is not configured.
     * @retval ::ids::IdsSensorErrc::InvalidConfiguration A specifier built from @p portPrefix does
     *                                                  not resolve to a configured security event.
     * @details All reporters are built here, up front, so that no detection ever pays for
     *          resolving a specifier.
     */
    static ara::core::Result<std::unique_ptr<IdsmDispatcher>> Create(const std::string &portPrefix);

    /**
     * @brief Release the reporters.
     * @return None.
     */
    ~IdsmDispatcher() override;

    /**
     * @copydoc ::ids::ISecurityEventDispatcher::Dispatch(TrafficClass, const SecurityEventContext &, SecurityEventTimestamp)
     */
    bool Dispatch(TrafficClass trafficClass, const SecurityEventContext &context, SecurityEventTimestamp timestamp) override;

  private:
    struct Impl; ///< The reporters, kept out of this header.

    /**
     * @brief Construct from already built reporters.
     * @param[in,out] impl Built reporters, taken over by this dispatcher.
     * @return None.
     * @details Private so that Create() is the only way in.
     */
    explicit IdsmDispatcher(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl; ///< Owned reporters.
};

} // namespace ids

/** @}*/ // End of IdsSensorEventDispatch
