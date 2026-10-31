/**
 * @file      UtilErrorDomain.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Error domain of the util helpers.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   A helper in @c util must not borrow the error domain of a component that uses it,
 *            otherwise the dependency runs the wrong way: ::ids::util::SlidingWindow would have to
 *            include the ids_sensor domain, and the thing meant to be reusable would stop being
 *            reusable. One small domain of its own is the price.
 */
#pragma once

#include "src/ara/core/ErrorCode.hpp"
#include "src/ara/core/ErrorDomain.hpp"

/** @addtogroup UtilErrors Error domain
 *  @ingroup Util
 *  @{
 */

namespace ids
{
namespace util
{

/**
 * @brief Failures the util helpers can report.
 */
enum class UtilErrc : ara::core::ErrorDomain::CodeType
{
    ZeroCapacity = 1, ///< A container was asked for a capacity of zero.
};

/**
 * @brief Domain of the ::ids::util::UtilErrc codes.
 */
class UtilErrorDomain final : public ara::core::ErrorDomain
{
  public:
    using Errc = UtilErrc; ///< Enumeration this domain gives meaning to.

    /// Unique identifier of this domain, distinct from the ids_sensor and libecu ones.
    static constexpr ara::core::ErrorDomain::IdType DOMAIN_ID = 0x000000001D530003ULL;

    /**
     * @brief Construct the domain.
     * @return None.
     */
    constexpr UtilErrorDomain() noexcept : ara::core::ErrorDomain(DOMAIN_ID)
    {
    }

    /**
     * @copydoc ::ara::core::ErrorDomain::Name() const
     */
    const char *Name() const noexcept override
    {
        return "Util";
    }

    /**
     * @copydoc ::ara::core::ErrorDomain::Message(ara::core::ErrorDomain::CodeType) const
     */
    const char *Message(ara::core::ErrorDomain::CodeType errorCode) const noexcept override
    {
        const char *text = "unknown util error";
        switch (static_cast<UtilErrc>(errorCode))
        {
            case UtilErrc::ZeroCapacity:
                text = "capacity of zero is not usable";
                break;
            default:
                // Keeps the generic message, so an unmapped value never yields nullptr.
                break;
        }
        return text;
    }
};

/**
 * @brief The one instance of the util error domain.
 * @return Reference to the domain, which has static storage duration.
 */
const ara::core::ErrorDomain &GetUtilErrorDomain() noexcept;

/**
 * @brief Build an ara::core::ErrorCode from a util enumerator.
 * @param[in] code        Enumerator identifying the failure.
 * @param[in] supportData Optional vendor data.
 * @return The corresponding error code, bound to the util domain.
 */
inline ara::core::ErrorCode MakeErrorCode(UtilErrc code, ara::core::ErrorDomain::SupportDataType supportData = 0) noexcept
{
    return ara::core::ErrorCode(static_cast<ara::core::ErrorDomain::CodeType>(code), GetUtilErrorDomain(), supportData);
}

} // namespace util
} // namespace ids

/** @}*/ // End of UtilErrors
