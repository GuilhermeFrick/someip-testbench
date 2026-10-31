/**
 * @file      Types.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Identifier types of the SOME/IP layer, as the communication matrix declares them.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   These aliases exist so that a generated constant carries its MEANING in the type, and
 *            so that a signature says which identifier it wants. @c Skeleton(ServiceId, InstanceId)
 *            reads unambiguously; @c Skeleton(std::uint16_t, std::uint16_t) does not, and swapping
 *            the two arguments would compile.
 *
 * @details   They are aliases, not distinct types, because that is what the SOME/IP wire format
 *            gives us: every one of these is a 16-bit field. Making them strong types would be a
 *            larger change than the bench needs, and would fight the generated header.
 *
 * @details   Written by hand. The VALUES come from platform/comm_matrix.yaml, through
 *            tools/gen_config.py, and land in the generated CommMatrix.hpp. This file holds only
 *            the types.
 */
#pragma once

#include <cstdint>

/** @defgroup LibEcu libecu
 *  @brief Facade in the style of ara::com over the vsomeip stack.
 */

/** @addtogroup LibEcuTypes Identifier types
 *  @ingroup LibEcu
 *  @{
 */

namespace ids
{
namespace ecu
{

using ServiceId      = std::uint16_t; ///< SOME/IP service identifier.
using InstanceId     = std::uint16_t; ///< SOME/IP instance identifier of a service.
using EventId        = std::uint16_t; ///< SOME/IP event or notifier identifier.
using MethodId       = std::uint16_t; ///< SOME/IP method identifier.
using EventgroupId   = std::uint16_t; ///< SOME/IP eventgroup identifier.
using ApplicationId  = std::uint16_t; ///< vsomeip application identifier of a node.
using Port           = std::uint16_t; ///< Transport port a service is offered on.

} // namespace ecu
} // namespace ids

/** @}*/ // End of LibEcuTypes
