/**
 * @file      EcuErrorDomain.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Error domain of the libecu facade: the failures the SOME/IP layer can report.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   The errors here are the ones a node can actually hit, and most of them come from
 *            lessons already paid for. ::ids::ecu::EcuErrc::ServiceDiscoveryNotReady is named after
 *            the worst of them: the vsomeip Service Discovery fails SILENTLY when the multicast
 *            route is missing, looping through @c pending_sd_offers with no error and no warning.
 *
 * @details   @warning Three codes here are DECLARED but NOT YET PRODUCED by any code path, and
 *            each one is marked as such on its enumerator. An error code that nothing returns is
 *            documentation, not behaviour, and reading this enumeration as a list of guarantees
 *            would be wrong. In particular ::ids::ecu::EcuErrc::ServiceDiscoveryNotReady does NOT
 *            yet turn that silence into an error: ::ids::ecu::Skeleton offers the service and does
 *            not verify that it was announced. Until it does, a consumer timing out in
 *            ::ids::ecu::Proxy::WaitUntilAvailable is the only signal.
 */
#pragma once

#include <cstdint>

#include "src/ara/core/ErrorCode.hpp"
#include "src/ara/core/ErrorDomain.hpp"

/** @addtogroup LibEcuErrors Error domain
 *  @ingroup LibEcu
 *  @{
 */

namespace ids
{
namespace ecu
{

/**
 * @brief Failures the libecu facade can report.
 */
enum class EcuErrc : ara::core::ErrorDomain::CodeType
{
    ApplicationCreateFailed     = 1, ///< The vsomeip runtime did not hand out an application object.
    ApplicationInitFailed       = 2, ///< init() refused, almost always a configuration that does not name this application.
    AlreadyStarted              = 3, ///< Start() called twice on the same object.
    NotStarted                  = 4, ///< An operation that needs the running stack was called before Start().
    RegistrationTimeout         = 5, ///< The routing manager did not register this application in time.
    ServiceDiscoveryNotReady    = 6, ///< The service was offered but Service Discovery never announced it. NOT YET PRODUCED: Skeleton does not verify the announcement.
    ServiceUnavailableTimeout    = 7, ///< The consumer waited for the service and it never became available.
    SubscriptionTimeout         = 8, ///< The subscription was requested and never acknowledged. NOT YET PRODUCED: Proxy::Subscribe does not wait for the acknowledgement.
    EventNotOffered             = 9, ///< Send() on an event that was not offered first.
    PayloadTooLarge             = 10, ///< The payload exceeds what one SOME/IP message may carry.
    InvalidConfiguration        = 11, ///< An identifier is zero, or the application name is empty.
    ServiceNotDeclared          = 12, ///< An operation named a service instance that ::ids::ecu::NodeRuntime was never told about.
    ServiceAlreadyDeclared      = 13, ///< The same service instance was declared twice on one ::ids::ecu::NodeRuntime.
};

/**
 * @brief Domain of the ::ids::ecu::EcuErrc codes.
 */
class EcuErrorDomain final : public ara::core::ErrorDomain
{
  public:
    using Errc = EcuErrc; ///< Enumeration this domain gives meaning to.

    /// Unique identifier of this domain, distinct from the ids_sensor one.
    static constexpr ara::core::ErrorDomain::IdType DOMAIN_ID = 0x000000001D530002ULL;

    /**
     * @brief Construct the domain.
     * @return None.
     */
    constexpr EcuErrorDomain() noexcept : ara::core::ErrorDomain(DOMAIN_ID)
    {
    }

    /**
     * @copydoc ::ara::core::ErrorDomain::Name() const
     */
    const char *Name() const noexcept override
    {
        return "LibEcu";
    }

    /**
     * @copydoc ::ara::core::ErrorDomain::Message(ara::core::ErrorDomain::CodeType) const
     */
    const char *Message(ara::core::ErrorDomain::CodeType errorCode) const noexcept override;
};

/**
 * @brief The one instance of the libecu error domain.
 * @return Reference to the domain, which has static storage duration.
 */
const ara::core::ErrorDomain &GetEcuErrorDomain() noexcept;

/**
 * @brief Build an ara::core::ErrorCode from a libecu enumerator.
 * @param[in] code        Enumerator identifying the failure.
 * @param[in] supportData Optional vendor data, such as a vsomeip state value.
 * @return The corresponding error code, bound to the libecu domain.
 * @details Found by argument-dependent lookup from the templated ara::core::ErrorCode constructor.
 */
inline ara::core::ErrorCode MakeErrorCode(EcuErrc code, ara::core::ErrorDomain::SupportDataType supportData = 0) noexcept
{
    return ara::core::ErrorCode(static_cast<ara::core::ErrorDomain::CodeType>(code), GetEcuErrorDomain(), supportData);
}

} // namespace ecu
} // namespace ids

/** @}*/ // End of LibEcuErrors
