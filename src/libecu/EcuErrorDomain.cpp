/**
 * @file      EcuErrorDomain.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Messages of the libecu error domain, and its singleton.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 */
#include "src/libecu/EcuErrorDomain.hpp"

namespace ids
{
namespace ecu
{

const char *EcuErrorDomain::Message(ara::core::ErrorDomain::CodeType errorCode) const noexcept
{
    const char *text = "unknown libecu error";
    switch (static_cast<EcuErrc>(errorCode))
    {
        case EcuErrc::ApplicationCreateFailed:
            text = "vsomeip runtime did not create the application";
            break;
        case EcuErrc::ApplicationInitFailed:
            text = "application init failed; check that the configuration names this application";
            break;
        case EcuErrc::AlreadyStarted:
            text = "already started";
            break;
        case EcuErrc::NotStarted:
            text = "not started";
            break;
        case EcuErrc::RegistrationTimeout:
            text = "routing manager did not register the application in time";
            break;
        case EcuErrc::ServiceDiscoveryNotReady:
            text = "service offered but Service Discovery never announced it; check the multicast route";
            break;
        case EcuErrc::ServiceUnavailableTimeout:
            text = "service did not become available in time";
            break;
        case EcuErrc::SubscriptionTimeout:
            text = "subscription was not acknowledged in time";
            break;
        case EcuErrc::EventNotOffered:
            text = "event was not offered before sending";
            break;
        case EcuErrc::PayloadTooLarge:
            text = "payload exceeds the maximum SOME/IP message size";
            break;
        case EcuErrc::InvalidConfiguration:
            text = "identifier is zero or the application name is empty";
            break;
        case EcuErrc::ServiceNotDeclared:
            text = "service instance was not declared on this runtime";
            break;
        case EcuErrc::ServiceAlreadyDeclared:
            text = "service instance was declared twice on this runtime";
            break;
        default:
            // Keeps the generic message, so an unmapped value never yields nullptr.
            break;
    }
    return text;
}

const ara::core::ErrorDomain &GetEcuErrorDomain() noexcept
{
    static const EcuErrorDomain domain;
    return domain;
}

} // namespace ecu
} // namespace ids
