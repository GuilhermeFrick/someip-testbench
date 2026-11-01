/**
 * @file      CoreErrorDomain.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Error domain of ara::core, as specified by the Adaptive Platform.
 * @version   1.0
 * @date      2026-11-01
 * @copyright Copyright (c) 2026
 *
 * @details   BENCH SUBSET of the AUTOSAR Adaptive Platform. See Span.hpp for why this directory
 *            exists.
 *
 * @details   This domain exists because ara::core must not borrow a consumer's error domain. An
 *            ara::core::InstanceSpecifier that refuses a malformed path has to report it in terms
 *            of ara::core, otherwise the platform subset would depend on the application that
 *            happens to use it. The same reasoning produced ::ids::UtilErrorDomain for
 *            @c src/util.
 *
 * @details   The enumerators and their values follow the specification, which is why they carry
 *            the specified @c k prefix while the rest of this project does not: a name here is an
 *            interface towards code written against the Adaptive Platform, not a local choice.
 *            Only the ones this subset can actually produce are declared; adding the rest would
 *            mean declaring error codes that nothing returns.
 */
#pragma once

#include <cstdint>

#include "src/ara/core/ErrorCode.hpp"
#include "src/ara/core/ErrorDomain.hpp"

/** @addtogroup AraCoreErrors ara::core error domain
 *  @ingroup AraCore
 *  @{
 */

namespace ara
{
namespace core
{

/**
 * @brief Errors ara::core can report.
 */
enum class CoreErrc : ErrorDomain::CodeType
{
    kInvalidArgument           = 22,  ///< An argument is not usable.
    kInvalidMetaModelShortname = 137, ///< A path element is not a well formed short name.
    kInvalidMetaModelPath      = 138, ///< The path as a whole is not a well formed meta-model path.
};

/**
 * @brief Domain of the ::ara::core::CoreErrc codes.
 */
class CoreErrorDomain final : public ErrorDomain
{
  public:
    using Errc = CoreErrc; ///< Enumeration this domain gives meaning to.

    /// Identifier of this domain, as the specification assigns it. It sits in the half of the
    /// range reserved to AUTOSAR, which is why it does not look like the project-defined domains.
    static constexpr ErrorDomain::IdType DOMAIN_ID = 0x8000000000000014ULL;

    /**
     * @brief Construct the domain.
     * @return None.
     * @details There is one instance, with static storage duration, reached through
     *          GetCoreErrorDomain().
     */
    constexpr CoreErrorDomain() noexcept : ErrorDomain(DOMAIN_ID)
    {
    }

    /**
     * @copydoc ::ara::core::ErrorDomain::Name() const
     */
    const char *Name() const noexcept override
    {
        return "Core";
    }

    /**
     * @copydoc ::ara::core::ErrorDomain::Message(ara::core::ErrorDomain::CodeType) const
     */
    const char *Message(ErrorDomain::CodeType errorCode) const noexcept override
    {
        const char *text = "unknown ara::core error";
        switch (static_cast<CoreErrc>(errorCode))
        {
            case CoreErrc::kInvalidArgument:
                text = "invalid argument";
                break;
            case CoreErrc::kInvalidMetaModelShortname:
                text = "invalid meta-model short name";
                break;
            case CoreErrc::kInvalidMetaModelPath:
                text = "invalid meta-model path";
                break;
            default:
                // Keeps the generic message, so an unmapped value never yields nullptr.
                break;
        }
        return text;
    }
};

/**
 * @brief The one instance of the ara::core error domain.
 * @return Reference to the domain, which has static storage duration.
 */
const ErrorDomain &GetCoreErrorDomain() noexcept;

/**
 * @brief Build an ara::core::ErrorCode from an ara::core enumerator.
 * @param[in] code        Enumerator identifying the error.
 * @param[in] supportData Optional vendor data.
 * @return The corresponding error code, bound to the ara::core domain.
 * @details Found by argument-dependent lookup from the templated ara::core::ErrorCode constructor.
 */
inline ErrorCode MakeErrorCode(CoreErrc code, ErrorDomain::SupportDataType supportData = 0) noexcept
{
    return ErrorCode(static_cast<ErrorDomain::CodeType>(code), GetCoreErrorDomain(), supportData);
}

} // namespace core
} // namespace ara

/** @}*/ // End of AraCoreErrors
