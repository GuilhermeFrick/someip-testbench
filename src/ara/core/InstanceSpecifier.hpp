/**
 * @file      InstanceSpecifier.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Meta-model identifier of a port instance, as specified by ara::core.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   BENCH SUBSET of the AUTOSAR Adaptive Platform. See Span.hpp for why this directory
 *            exists.
 *
 * @details   This is how Adaptive Platform code names a configured thing without hard-coding its
 *            identifiers. An application passes a specifier such as
 *            @c "ids_sensor/SecurityEvents/MalformedMessage" and the platform resolves it, through
 *            the meta-model, to the concrete event definition and its filter chain. It is the same
 *            principle as the bench communication matrix: identifiers live in configuration, not
 *            in code.
 *
 * @details   Deviation from the specification, deliberate: the constructor and Create() take
 *            @c const @c std::string @c & rather than @c ara::core::StringView, which is not part
 *            of this subset. Create() is the preferred form, because it reports a malformed
 *            specifier through ara::core::Result instead of throwing.
 */
#pragma once

#include <string>

#include "src/ara/core/Result.hpp"

/** @addtogroup AraCoreInstanceSpecifier ara::core::InstanceSpecifier
 *  @ingroup AraCore
 *  @{
 */

namespace ara
{
namespace core
{

/**
 * @brief Identifies one port instance in the meta-model, by path.
 */
class InstanceSpecifier final
{
  public:
    /**
     * @brief Build a specifier, reporting a malformed path as an error.
     * @param[in] metaModelIdentifier Meta-model path of the port instance.
     * @return The specifier on success, or an error when @p metaModelIdentifier is empty or
     *         otherwise not a well formed path.
     */
    static Result<InstanceSpecifier> Create(const std::string &metaModelIdentifier);

    /**
     * @brief Meta-model path of this specifier.
     * @return The path, exactly as given at construction.
     */
    const std::string &ToString() const noexcept
    {
        return m_metaModelIdentifier;
    }

    /**
     * @brief Compare two specifiers.
     * @param[in] other Specifier to compare against.
     * @return Whether both denote the same port instance.
     * @retval true  The paths are identical.
     * @retval false The paths differ.
     */
    bool operator==(const InstanceSpecifier &other) const noexcept
    {
        return (m_metaModelIdentifier == other.m_metaModelIdentifier);
    }

    /**
     * @brief Compare two specifiers for inequality.
     * @param[in] other Specifier to compare against.
     * @return Whether the specifiers denote different port instances.
     * @retval true  The paths differ.
     * @retval false The paths are identical.
     */
    bool operator!=(const InstanceSpecifier &other) const noexcept
    {
        return !(*this == other);
    }

  private:
    /**
     * @brief Construct from an already validated path.
     * @param[in] metaModelIdentifier Meta-model path, validated by Create().
     * @return None.
     * @details Private so that Create() is the only way in, which is what guarantees every
     *          specifier in the system was validated.
     */
    explicit InstanceSpecifier(const std::string &metaModelIdentifier) : m_metaModelIdentifier(metaModelIdentifier)
    {
    }

    std::string m_metaModelIdentifier; ///< Meta-model path of the port instance.
};

} // namespace core
} // namespace ara

/** @}*/ // End of AraCoreInstanceSpecifier
