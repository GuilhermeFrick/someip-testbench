/**
 * @file      EventReporter.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Sensor output: the typed Security Event and the reporting interface.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   The interface exists because the destination genuinely varies: on the bench it is a
 *            structured log; on an AUTOSAR Adaptive target it is @c ara::idsm; in the tests it is
 *            an in-memory collector. The sensor must not know which one is active.
 */
#pragma once

#include <cstdint>

#include "src/ids_sensor/types.hpp"

/** @addtogroup IdsSensorEventReporter Security event reporting
 *  @ingroup IdsSensor
 *  @{
 */

namespace ids
{

/**
 * @brief Security event type identifiers.
 * @details Each value corresponds to a @c SecurityEventDefinition of the IdsM. The concrete values
 *          come from the platform configuration; they are not invented here.
 */
enum class SecurityEventId : std::uint32_t
{
    Dos                 = 1U, ///< Denial of service detected.
    MalformedMessage    = 2U, ///< Malformed or randomised payload, reported for the Fuzzy class.
    SpoofedNotification = 3U, ///< Man in the middle from a single source.
    MultiSourceSpoof    = 4U, ///< Man in the middle from multiple sources.
};

/**
 * @brief Context data carried by an event.
 * @details Fixed fields only, so reporting never allocates.
 */
struct EventContext
{
    std::uint32_t srcIp      = 0U;   ///< Source IPv4 address of the offending packet.
    std::uint16_t serviceId  = 0U;   ///< SOME/IP service identifier.
    std::uint16_t methodId   = 0U;   ///< SOME/IP method or event identifier.
    float         confidence = 0.0F; ///< Probability of the predicted class.
};

/**
 * @brief Abstract security event sink.
 */
class IEventReporter
{
  public:
    /**
     * @brief Virtual destructor.
     * @return None.
     */
    virtual ~IEventReporter() = default;

    /**
     * @brief Report one event.
     * @param[in] id      Type of event observed.
     * @param[in] ts      Timestamp in seconds, from the single scenario clock.
     * @param[in] context Context data of the offending packet.
     * @return None.
     * @details Filtering, aggregation and qualification belong to the IdsM. The sensor only reports
     *          what it observed.
     */
    virtual void Report(SecurityEventId id, double ts, const EventContext &context) = 0;
};

/**
 * @brief Translate a predicted class into an event type.
 * @param[in]  trafficClass Class predicted by the classifier.
 * @param[out] out          Corresponding event type, untouched when the class raises no event.
 * @return Whether the class raises an event.
 * @retval true  @p out holds the event type to report.
 * @retval false The class is ::ids::TrafficClass::Normal, which raises no event.
 */
bool TrafficClassToEvent(TrafficClass trafficClass, SecurityEventId &out);

} // namespace ids

/** @}*/ // End of IdsSensorEventReporter
