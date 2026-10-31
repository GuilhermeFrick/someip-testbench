/**
 * @file      ErrorCode.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Typed error identity: a value plus the domain it belongs to, as specified by ara::core.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   BENCH SUBSET of the AUTOSAR Adaptive Platform. See Span.hpp for why this directory
 *            exists.
 *
 * @details   An ErrorCode is built from a project enumerator, not from a string. A component
 *           declares its enumeration plus a @c MakeErrorCode overload in its own namespace, and
 *            the templated constructor below finds that overload by argument-dependent lookup. The
 *            caller then compares against the enumerator, which the compiler checks, instead of
 *            comparing message text.
 *
 * @details   Deviations from the specification, all deliberate:
 *            - Message() returns @c const @c char @c * rather than @c ara::core::StringView, which
 *              is not part of this subset;
 *            - @c ThrowAsException() is not implemented. Exceptions are not the signalling
 *              mechanism here, which is also what the C++ code standard requires.
 */
#pragma once

#include <type_traits>

#include "src/ara/core/ErrorDomain.hpp"

/** @addtogroup AraCoreErrorCode ara::core::ErrorCode
 *  @ingroup AraCore
 *  @{
 */

namespace ara
{
namespace core
{

/**
 * @brief An error value together with the domain that gives it meaning.
 */
class ErrorCode final
{
  public:
    /**
     * @brief Construct from a value and an explicit domain.
     * @param[in] value       Error value within @p domain.
     * @param[in] domain      Domain the value belongs to, which must have static storage duration.
     * @param[in] supportData Optional vendor data, such as a platform errno.
     * @return None.
     */
    constexpr ErrorCode(ErrorDomain::CodeType value, const ErrorDomain &domain, ErrorDomain::SupportDataType supportData = 0) noexcept
        : m_value(value), m_domain(&domain), m_supportData(supportData)
    {
    }

    /**
     * @brief Construct from a project enumerator.
     * @tparam EnumT Enumeration type of the component declaring the error.
     * @param[in] errorEnum   Enumerator identifying the error.
     * @param[in] supportData Optional vendor data, such as a platform errno.
     * @return None.
     * @details Resolves the domain through a @c MakeErrorCode overload found by
     *          argument-dependent lookup in the namespace of @p EnumT.
     */
    template <typename EnumT, typename = typename std::enable_if<std::is_enum<EnumT>::value>::type>
    constexpr ErrorCode(EnumT errorEnum, ErrorDomain::SupportDataType supportData = 0) noexcept
        : ErrorCode(MakeErrorCode(errorEnum, supportData))
    {
    }

    /**
     * @brief Error value.
     * @return The value, meaningful only together with Domain().
     */
    constexpr ErrorDomain::CodeType Value() const noexcept
    {
        return m_value;
    }

    /**
     * @brief Domain of this code.
     * @return Reference to the owning domain.
     */
    constexpr const ErrorDomain &Domain() const noexcept
    {
        return *m_domain;
    }

    /**
     * @brief Vendor data carried with this code.
     * @return The support data, or 0 when none was given.
     */
    constexpr ErrorDomain::SupportDataType SupportData() const noexcept
    {
        return m_supportData;
    }

    /**
     * @brief Human readable message for this code.
     * @return Null-terminated message supplied by the domain, never nullptr.
     */
    const char *Message() const noexcept
    {
        return m_domain->Message(m_value);
    }

    /**
     * @brief Compare two codes.
     * @param[in] other Code to compare against.
     * @return Whether both denote the same error.
     * @retval true  Same domain and same value.
     * @retval false Different domain or different value.
     */
    constexpr bool operator==(const ErrorCode &other) const noexcept
    {
        return ((m_value == other.m_value) && (*m_domain == *other.m_domain));
    }

    /**
     * @brief Compare two codes for inequality.
     * @param[in] other Code to compare against.
     * @return Whether the codes denote different errors.
     * @retval true  Different domain or different value.
     * @retval false Same domain and same value.
     */
    constexpr bool operator!=(const ErrorCode &other) const noexcept
    {
        return !(*this == other);
    }

  private:
    ErrorDomain::CodeType        m_value;       ///< Error value within the domain.
    const ErrorDomain           *m_domain;      ///< Owning domain, with static storage duration.
    ErrorDomain::SupportDataType m_supportData; ///< Optional vendor data.
};

} // namespace core
} // namespace ara

/** @}*/ // End of AraCoreErrorCode
