/**
 * @file      IdsSensorErrorDomain.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Error domain of the ids_sensor component: the errors its operations can report.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   This is what lets the component report failures as TYPED codes rather than as text.
 *            The C++ code standard asks for stable failure strings that tests assert verbatim; the
 *            Adaptive Platform asks for an ara::core::ErrorCode carrying a domain. The second form
 *            is used here, because the caller then compares against an enumerator the compiler
 *            checks, and the message stays a diagnostic rather than becoming an interface.
 *
 * @details   A component declares its enumeration, its domain, and a @c MakeErrorCode overload in
 *            its own namespace. The templated ara::core::ErrorCode constructor finds that overload
 *            by argument-dependent lookup, which is what makes @c return IdsSensorErrc::X compile
 *            straight into a Result.
 */
#pragma once

#include <cstdint>

#include "src/ara/core/ErrorCode.hpp"
#include "src/ara/core/ErrorDomain.hpp"

/** @addtogroup IdsSensorErrors Error domain
 *  @ingroup IdsSensor
 *  @{
 */

namespace ids
{

/**
 * @brief Errors the ids_sensor component can report.
 */
enum class IdsSensorErrc : ara::core::ErrorDomain::CodeType
{
    ArtefactNotFound     = 1, ///< The artefact file does not exist at the given path.
    ArtefactUnreadable   = 2, ///< The artefact exists but could not be read; see the support data.
    ArtefactCorrupted    = 3, ///< The artefact was read but its contents do not match the expected layout.
    TrainingRunMismatch  = 4, ///< The artefacts do not all come from the same training run.
    ModelNotAvailable    = 5, ///< No tree model is compiled into this build.
    InvalidConfiguration = 6, ///< A window size or capacity is zero, or otherwise unusable.
    MalformedPacket      = 7, ///< The frame is not a well formed SOME/IP message.
};

/**
 * @brief Domain of the ::ids::IdsSensorErrc codes.
 */
class IdsSensorErrorDomain final : public ara::core::ErrorDomain
{
  public:
    using Errc = IdsSensorErrc; ///< Enumeration this domain gives meaning to.

    /// Unique identifier of this domain. Project-defined domains take identifiers from the lower
    /// half of the range; the specification leaves the upper half, with the high bit set, to
    /// AUTOSAR itself.
    static constexpr ara::core::ErrorDomain::IdType DOMAIN_ID = 0x000000001D530001ULL;

    /**
     * @brief Construct the domain.
     * @return None.
     * @details There is one instance, with static storage duration, reached through
     *          GetIdsSensorErrorDomain().
     */
    constexpr IdsSensorErrorDomain() noexcept : ara::core::ErrorDomain(DOMAIN_ID)
    {
    }

    /**
     * @copydoc ::ara::core::ErrorDomain::Name() const
     */
    const char *Name() const noexcept override
    {
        return "IdsSensor";
    }

    /**
     * @copydoc ::ara::core::ErrorDomain::Message(ara::core::ErrorDomain::CodeType) const
     */
    const char *Message(ara::core::ErrorDomain::CodeType errorCode) const noexcept override
    {
        const char *text = "unknown ids_sensor error";
        switch (static_cast<IdsSensorErrc>(errorCode))
        {
            case IdsSensorErrc::ArtefactNotFound:
                text = "artefact file not found";
                break;
            case IdsSensorErrc::ArtefactUnreadable:
                text = "artefact file could not be read";
                break;
            case IdsSensorErrc::ArtefactCorrupted:
                text = "artefact contents do not match the expected layout";
                break;
            case IdsSensorErrc::TrainingRunMismatch:
                text = "artefacts come from different training runs";
                break;
            case IdsSensorErrc::ModelNotAvailable:
                text = "no tree model is compiled into this build";
                break;
            case IdsSensorErrc::InvalidConfiguration:
                text = "window size or capacity is unusable";
                break;
            case IdsSensorErrc::MalformedPacket:
                text = "frame is not a well formed SOME/IP message";
                break;
            default:
                // Keeps the generic message, so an unmapped value never yields nullptr.
                break;
        }
        return text;
    }
};

/**
 * @brief The one instance of the ids_sensor error domain.
 * @return Reference to the domain, which has static storage duration.
 */
const ara::core::ErrorDomain &GetIdsSensorErrorDomain() noexcept;

/**
 * @brief Build an ara::core::ErrorCode from an ids_sensor enumerator.
 * @param[in] code        Enumerator identifying the error.
 * @param[in] supportData Optional vendor data, such as the platform errno behind an I/O failure.
 * @return The corresponding error code, bound to the ids_sensor domain.
 * @details Found by argument-dependent lookup from the templated ara::core::ErrorCode constructor.
 */
inline ara::core::ErrorCode MakeErrorCode(IdsSensorErrc code, ara::core::ErrorDomain::SupportDataType supportData = 0) noexcept
{
    return ara::core::ErrorCode(static_cast<ara::core::ErrorDomain::CodeType>(code), GetIdsSensorErrorDomain(), supportData);
}

} // namespace ids

/** @}*/ // End of IdsSensorErrors
