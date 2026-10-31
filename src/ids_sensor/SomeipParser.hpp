/**
 * @file      SomeipParser.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Layered packet decoder: Ethernet to IP to TCP/UDP to SOME/IP to Service Discovery.
 * @version   2.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   Concrete and STATELESS: the result depends only on the frame. That is what makes it
 *            the easiest stage to validate, and therefore the first one to close parity against
 *            the Python extractor.
 */
#pragma once

#include "src/ara/core/Result.hpp"
#include "src/ids_sensor/IdsSensorErrorDomain.hpp"
#include "src/ids_sensor/types.hpp"

/** @addtogroup IdsSensorParser SOME/IP parser
 *  @ingroup IdsSensor
 *  @{
 */

namespace ids
{

/**
 * @brief Decodes a raw frame into a ::ids::ParsedPacket.
 */
class SomeipParser final
{
  public:
    /**
     * @brief Decode one frame.
     * @param[in] frame Raw frame to decode.
     * @return The decoded packet, or an error when the frame does not carry a well formed SOME/IP
     *         message.
     * @retval ::ids::IdsSensorErrc::MalformedPacket The frame is not SOME/IP, or a length field is
     *                                              inconsistent with the bytes actually present.
     * @warning The payload views in the returned packet point INTO the buffer of @p frame. Nothing
     *          is copied, and they do not outlive the frame.
     */
    ara::core::Result<ParsedPacket> Parse(const Frame &frame) const;
};

} // namespace ids

/** @}*/ // End of IdsSensorParser
