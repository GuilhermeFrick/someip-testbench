/**
 * @file      EventReporter.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Reporter of one security event type towards the Intrusion Detection System Manager,
 *            as specified by ara::idsm.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   BENCH SUBSET of the AUTOSAR Adaptive Platform. See ../core/Span.hpp for why this
 *            directory exists.
 *
 * @details   The design point that matters: ONE reporter per security event type. The identity of
 *            the event comes from the ara::core::InstanceSpecifier given at construction, which
 *            the meta-model resolves to a @c SecurityEventDefinition and to the filter chain bound
 *            by its @c SecurityEventContextMapping. It is NOT a parameter of ReportEvent().
 *
 *            That is deliberate in the specification. Because the event type is fixed per reporter,
 *            the platform can resolve the filter chain once, at construction, and the reporting
 *            call stays cheap and allocation free. A reporter that took the event type per call
 *            would have to resolve it on every call, on the detection path.
 *
 * @details   The IdsM is what filters, aggregates and qualifies. The reporting side only states
 *            what it observed, which is why ReportEvent() has no return value and cannot fail from
 *            the caller's point of view.
 *
 * @details   Deviations from the specification, all deliberate:
 *            - @c ContextDataType is a view (ara::core::Span) rather than
 *              @c ara::core::Vector<std::uint8_t>, so that reporting never allocates;
 *            - the @c ReportEvent(TimestampType, CountType) overload, the one without context
 *              data, is omitted. With @c CountType and @c TimestampType both being unsigned
 *              integers, it makes calls with integer literals ambiguous for no benefit here.
 */
#pragma once

#include <cstdint>
#include <memory>

#include "src/ara/core/InstanceSpecifier.hpp"
#include "src/ara/core/Result.hpp"
#include "src/ara/core/Span.hpp"

/** @defgroup AraIdsm ara::idsm
 *  @brief Bench subset of the AUTOSAR Adaptive Platform intrusion detection interface.
 */

/** @addtogroup AraIdsmEventReporter ara::idsm::EventReporter
 *  @ingroup AraIdsm
 *  @{
 */

namespace ara
{
namespace idsm
{

using ContextDataType = core::Span<const std::uint8_t>; ///< Opaque context bytes, laid out by the meta-model.
using CountType       = std::uint32_t;                  ///< How many occurrences one call reports.
using TimestampType   = std::uint64_t;                  ///< Event time, in the unit the platform configures.

/**
 * @brief Reports occurrences of ONE security event type to the IdsM.
 */
class EventReporter final
{
  public:
    /**
     * @brief Build a reporter for the event the specifier names.
     * @param[in] specifier Meta-model path of the security event definition to report.
     * @return The reporter on success, or an error when the specifier does not resolve to a
     *         configured security event.
     * @details Preferred over the constructor, because it reports a configuration mistake through
     *          ara::core::Result instead of leaving a reporter that silently drops events.
     */
    static core::Result<EventReporter> Create(const core::InstanceSpecifier &specifier);

    /**
     * @brief Release the reporter.
     * @return None.
     */
    ~EventReporter();

    /**
     * @brief Move constructor.
     * @param[in,out] other Reporter to move from.
     * @return None.
     */
    EventReporter(EventReporter &&other) noexcept;

    /**
     * @brief Move assignment.
     * @param[in,out] other Reporter to move from.
     * @return Reference to this reporter.
     */
    EventReporter &operator=(EventReporter &&other) noexcept;

    /**
     * @brief Copy construction is deleted: a reporter owns a platform binding.
     * @return None.
     */
    EventReporter(const EventReporter &) = delete;

    /**
     * @brief Copy assignment is deleted: a reporter owns a platform binding.
     * @return Nothing; the operator cannot be called.
     */
    EventReporter &operator=(const EventReporter &) = delete;

    /**
     * @brief Report occurrences of this event, without context data.
     * @param[in] count Number of occurrences being reported.
     * @return None.
     */
    void ReportEvent(CountType count = 1U) noexcept;

    /**
     * @brief Report occurrences of this event, with context data.
     * @param[in] contextData Opaque context bytes, laid out as the meta-model defines.
     * @param[in] count       Number of occurrences being reported.
     * @return None.
     * @warning The reporter copies whatever it needs before returning, so @p contextData need not
     *          outlive the call.
     */
    void ReportEvent(const ContextDataType &contextData, CountType count = 1U) noexcept;

    /**
     * @brief Report occurrences of this event, with context data and an explicit timestamp.
     * @param[in] contextData Opaque context bytes, laid out as the meta-model defines.
     * @param[in] timestamp   Time of the occurrence, in the configured unit.
     * @param[in] count       Number of occurrences being reported.
     * @return None.
     * @details This is the form the sensor uses, because the bench carries a single scenario clock
     *          and every stage timestamps against it.
     */
    void ReportEvent(const ContextDataType &contextData, TimestampType timestamp, CountType count = 1U) noexcept;

  private:
    struct Impl; ///< Platform binding, kept out of this header.

    /**
     * @brief Construct from an already resolved platform binding.
     * @param[in,out] impl Resolved binding, taken over by this reporter.
     * @return None.
     * @details Private so that Create() is the only way in.
     */
    explicit EventReporter(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl; ///< Owned platform binding.
};

} // namespace idsm
} // namespace ara

/** @}*/ // End of AraIdsmEventReporter
