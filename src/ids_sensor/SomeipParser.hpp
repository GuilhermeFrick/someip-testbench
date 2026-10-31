/**
 * @file      SomeipParser.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Layered packet decoder: Ethernet to IP to TCP/UDP to SOME/IP to Service Discovery.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   Concrete and STATELESS: the result depends only on the frame. That is what makes it
 *            the easiest stage to validate, and therefore the first one to close parity against
 *            the Python extractor.
 */
#pragma once

#include "src/ids_sensor/types.hpp"

/** @addtogroup IdsSensorParser SOME/IP parser
 *  @ingroup IdsSensor
 *  @{
 */

namespace ids
{

/**
 * @brief Decodes a raw frame into a ParsedPacket.
 */
class SomeipParser final
{
  public:
    /**
     * @brief Decode one frame.
     * @param[in]  frame Raw frame to decode.
     * @param[out] out   Decoded packet. On failure its @c valid field is set to false.
     * @return Whether the frame carried a well formed SOME/IP message.
     * @retval true  The frame is SOME/IP and @p out is populated.
     * @retval false The frame is not SOME/IP or is malformed.
     * @warning The payloads in @p out are VIEWS into the buffer of @p frame. Nothing is copied,
     *          and they do not outlive the frame.
     */
    [[nodiscard]] bool Parse(const Frame &frame, ParsedPacket &out) const;
};

} // namespace ids

/** @}*/ // End of IdsSensorParser
