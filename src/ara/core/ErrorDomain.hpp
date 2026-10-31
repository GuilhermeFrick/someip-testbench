/**
 * @file      ErrorDomain.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Namespace for error codes, as specified by ara::core.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   BENCH SUBSET of the AUTOSAR Adaptive Platform. See Span.hpp for why this directory
 *            exists.
 *
 * @details   A domain is what makes an error code identifiable without strings. Two components may
 *            both use the value 1 for an error; the domain says whose 1 it is. Each domain carries
 *            a unique 64-bit identifier, and the specification reserves ranges for AUTOSAR itself,
 *            so a project-defined domain picks an identifier outside them.
 *
 * @details   Deviation from the specification, deliberate: @c SupportDataType is fixed to a signed
 *            32-bit integer rather than being implementation-defined, which is enough to carry a
 *            platform errno alongside the code.
 */
#pragma once

#include <cstdint>

/** @defgroup AraCore ara::core
 *  @brief Bench subset of the AUTOSAR Adaptive Platform core types.
 */

/** @addtogroup AraCoreErrorDomain ara::core::ErrorDomain
 *  @ingroup AraCore
 *  @{
 */

namespace ara
{
namespace core
{

/**
 * @brief Identifies a set of error codes and maps each one to a message.
 * @details Instances are stateless singletons with static storage duration. Copying and moving are
 *          deleted so that an ErrorCode can hold a reference to a domain and compare domains by
 *          address and identifier.
 */
class ErrorDomain
{
  public:
    using IdType          = std::uint64_t; ///< Unique identifier of a domain.
    using CodeType        = std::int32_t;  ///< Error value within a domain.
    using SupportDataType = std::int32_t;  ///< Optional vendor data carried with a code.

    /**
     * @brief Copy construction is deleted.
     * @return None.
     */
    ErrorDomain(const ErrorDomain &) = delete;

    /**
     * @brief Move construction is deleted.
     * @return None.
     */
    ErrorDomain(ErrorDomain &&) = delete;

    /**
     * @brief Copy assignment is deleted.
     * @return Nothing; the operator cannot be called.
     */
    ErrorDomain &operator=(const ErrorDomain &) = delete;

    /**
     * @brief Move assignment is deleted.
     * @return Nothing; the operator cannot be called.
     */
    ErrorDomain &operator=(ErrorDomain &&) = delete;

    /**
     * @brief Compare two domains.
     * @param[in] other Domain to compare against.
     * @return Whether both denote the same domain.
     * @retval true  Both have the same identifier.
     * @retval false The identifiers differ.
     */
    constexpr bool operator==(const ErrorDomain &other) const noexcept
    {
        return (m_id == other.m_id);
    }

    /**
     * @brief Compare two domains for inequality.
     * @param[in] other Domain to compare against.
     * @return Whether the domains differ.
     * @retval true  The identifiers differ.
     * @retval false Both have the same identifier.
     */
    constexpr bool operator!=(const ErrorDomain &other) const noexcept
    {
        return (m_id != other.m_id);
    }

    /**
     * @brief Identifier of this domain.
     * @return The unique 64-bit identifier.
     */
    constexpr IdType Id() const noexcept
    {
        return m_id;
    }

    /**
     * @brief Short name of this domain.
     * @return Null-terminated name, for diagnostics.
     */
    virtual const char *Name() const noexcept = 0;

    /**
     * @brief Message for one error value of this domain.
     * @param[in] errorCode Value to describe.
     * @return Null-terminated message, never nullptr. An unknown value yields a generic message.
     */
    virtual const char *Message(CodeType errorCode) const noexcept = 0;

  protected:
    /**
     * @brief Construct a domain with its identifier.
     * @param[in] id Unique identifier, outside the ranges the specification reserves for AUTOSAR.
     * @return None.
     */
    explicit constexpr ErrorDomain(IdType id) noexcept : m_id(id)
    {
    }

    /**
     * @brief Destructor, protected and non-virtual because domains are never owned polymorphically.
     * @return None.
     */
    ~ErrorDomain() = default;

  private:
    IdType m_id; ///< Unique identifier of this domain.
};

} // namespace core
} // namespace ara

/** @}*/ // End of AraCoreErrorDomain
