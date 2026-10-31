/**
 * @file      SomeipParser.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Layered frame decoder, byte for byte as the Python reference does it.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   Mirrors @c parse() of @c src/someip.py in the reproduction repository. Four of its
 *            behaviours look like defects and are reproduced ANYWAY, because the model was trained
 *            on what that code produces. Each one is marked below, and "improving" any of them
 *            breaks parity:
 *
 *            1. a transport payload SHORTER than the 16-byte SOME/IP header is NOT a failure. The
 *               reference returns a packet with empty SOME/IP and SD payloads, and the extractor
 *               consumes it, producing a feature vector. Only a non-IPv4 frame or a protocol other
 *               than TCP and UDP yields nothing;
 *            2. VLAN-tagged frames are DROPPED. The reference compares the EtherType field at
 *               offset 12 against 0x0800 exactly, so an 802.1Q frame never reaches the IP layer.
 *               Handling the tag would make this parser see packets the reference never saw;
 *            3. the transport payload runs to the END OF THE CAPTURED FRAME, not to the length the
 *               IP header declares. Ethernet pads frames below 60 bytes, so that padding is part
 *               of the payload, and it is scored by the byte model and measured by the length
 *               features. Trimming to @c ip_len would change twelve of the sixteen features on
 *               every small packet;
 *            4. IPv6 and IP fragmentation are not handled at all.
 *
 * @details   ONE deliberate divergence: the reference does no bounds checking, so a frame too
 *            short to hold the headers raises IndexError and stops the run. Reading past the end
 *            is undefined behaviour in C++, not an exception, so short frames are rejected here
 *            with ::ids::IdsSensorErrc::MalformedPacket. The divergence can only show up on a
 *            frame where the reference would have crashed.
 */
#include "src/ids_sensor/SomeipParser.hpp"

namespace ids
{

namespace
{

/* Ethernet */
constexpr std::size_t   kEthHeaderBytes  = 14U;     ///< Bytes of the Ethernet II header.
constexpr std::size_t   kEtherTypeOffset = 12U;     ///< Offset of the EtherType field.
constexpr std::uint8_t  kEtherTypeIpv4Hi = 0x08U;   ///< First byte of the IPv4 EtherType.
constexpr std::uint8_t  kEtherTypeIpv4Lo = 0x00U;   ///< Second byte of the IPv4 EtherType.

/* IPv4, offsets relative to the start of the frame */
constexpr std::size_t  kIpVersionIhlOffset = 14U;  ///< Version and header length nibbles.
constexpr std::size_t  kIpTotalLengthOffset = 16U; ///< Total length of the datagram.
constexpr std::size_t  kIpProtocolOffset   = 23U;  ///< Transport protocol number.
constexpr std::size_t  kIpSourceOffset     = 26U;  ///< Source address.
constexpr std::size_t  kIpDestOffset       = 30U;  ///< Destination address.
constexpr std::uint8_t kIhlMask            = 0x0FU; ///< Low nibble holds the header length in words.
constexpr std::size_t  kIpWordBytes        = 4U;   ///< Bytes per IP header length unit.

/* Transport */
constexpr std::uint8_t kProtoTcp         = 6U;   ///< IP protocol number of TCP.
constexpr std::uint8_t kProtoUdp         = 17U;  ///< IP protocol number of UDP.
constexpr std::size_t  kUdpHeaderBytes   = 8U;   ///< Fixed size of the UDP header.
constexpr std::size_t  kTcpDataOffsetPos = 12U;  ///< Offset, within the TCP header, of the data offset nibble.

/* SOME/IP */
constexpr std::size_t   kSomeipHeaderBytes = 16U;    ///< Bytes of the SOME/IP header.
constexpr std::size_t   kServiceIdOffset   = 0U;     ///< Service identifier, within the SOME/IP header.
constexpr std::size_t   kMethodIdOffset    = 2U;     ///< Method or event identifier.
constexpr std::uint16_t kSdServiceId       = 0xFFFFU; ///< Service identifier that marks Service Discovery.
constexpr std::uint16_t kSdPort            = 30490U;  ///< UDP port of the Service Discovery.

/**
 * @brief Read a big-endian 16-bit field.
 * @param[in] frame  Frame to read from.
 * @param[in] offset Offset of the field; the caller guarantees two bytes are available.
 * @return The field value.
 */
std::uint16_t ReadBe16(const PayloadView &frame, std::size_t offset)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(frame[offset]) << 8U) |
                                      static_cast<std::uint16_t>(frame[offset + 1U]));
}

/**
 * @brief Read a big-endian 32-bit field, used for the IPv4 addresses.
 * @param[in] frame  Frame to read from.
 * @param[in] offset Offset of the field; the caller guarantees four bytes are available.
 * @return The field value.
 * @details The reference keeps addresses as dotted strings and uses them as dictionary keys. Here
 *          they are integers, which group flows and sources identically because the mapping is
 *          one to one, and which cost nothing to hash.
 */
std::uint32_t ReadBe32(const PayloadView &frame, std::size_t offset)
{
    return ((static_cast<std::uint32_t>(frame[offset]) << 24U) | (static_cast<std::uint32_t>(frame[offset + 1U]) << 16U) |
            (static_cast<std::uint32_t>(frame[offset + 2U]) << 8U) | static_cast<std::uint32_t>(frame[offset + 3U]));
}

} // namespace

ara::core::Result<ParsedPacket> SomeipParser::Parse(const Frame &frame) const
{
    using Res = ara::core::Result<ParsedPacket>;

    const PayloadView &bytes = frame.bytes;

    /* Bounds check that the reference does not have. Everything below indexes up
     * to kIpDestOffset + 4, so that much has to exist before anything is read. */
    if (bytes.size() < (kIpDestOffset + kIpWordBytes))
    {
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::MalformedPacket));
    }
    else
    {
        /* The Ethernet and IPv4 fixed headers are present. */
    }

    /* (2) EtherType compared exactly: an 802.1Q tagged frame carries 0x8100 here
     * and is dropped, exactly as the reference drops it. */
    if ((bytes[kEtherTypeOffset] != kEtherTypeIpv4Hi) || (bytes[kEtherTypeOffset + 1U] != kEtherTypeIpv4Lo))
    {
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::MalformedPacket));
    }
    else
    {
        /* IPv4. */
    }

    const std::size_t   ihlBytes = (static_cast<std::size_t>(bytes[kIpVersionIhlOffset] & kIhlMask) * kIpWordBytes);
    const std::uint8_t  protocol = bytes[kIpProtocolOffset];
    const std::uint16_t ipLen    = ReadBe16(bytes, kIpTotalLengthOffset);
    const std::size_t   l4Start  = (kEthHeaderBytes + ihlBytes);

    ParsedPacket out;
    out.ts         = frame.ts;
    out.ipLen      = ipLen;
    out.flow.proto = protocol;
    out.flow.src   = ReadBe32(bytes, kIpSourceOffset);
    out.flow.dst   = ReadBe32(bytes, kIpDestOffset);

    /* The transport header has to fit before its fields are read. */
    std::size_t payloadStart = 0U;
    if (protocol == kProtoUdp)
    {
        if (bytes.size() < (l4Start + kUdpHeaderBytes))
        {
            return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::MalformedPacket));
        }
        out.flow.sport = ReadBe16(bytes, l4Start);
        out.flow.dport = ReadBe16(bytes, l4Start + 2U);
        payloadStart   = (l4Start + kUdpHeaderBytes);
    }
    else if (protocol == kProtoTcp)
    {
        if (bytes.size() < (l4Start + kTcpDataOffsetPos + 1U))
        {
            return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::MalformedPacket));
        }
        out.flow.sport                 = ReadBe16(bytes, l4Start);
        out.flow.dport                 = ReadBe16(bytes, l4Start + 2U);
        const std::size_t dataOffBytes = (static_cast<std::size_t>(bytes[l4Start + kTcpDataOffsetPos] >> 4U) * kIpWordBytes);
        payloadStart                   = (l4Start + dataOffBytes);
    }
    else
    {
        /* Neither TCP nor UDP: the reference yields nothing for this frame. */
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::MalformedPacket));
    }

    if (payloadStart > bytes.size())
    {
        /* A declared header length that runs past the captured bytes. The reference
         * would slice to an empty payload here; rejecting is the safe equivalent and
         * only differs on a frame that is already inconsistent. */
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::MalformedPacket));
    }
    else
    {
        /* The payload offset is inside the frame. */
    }

    /* (3) To the END OF THE FRAME, not to ipLen. Ethernet padding on small frames
     * is part of this payload, and both the byte model and the length features see
     * it. Trimming to ipLen would change twelve of the sixteen features. */
    out.plL4 = bytes.subspan(payloadStart, bytes.size() - payloadStart);

    /* (1) A payload shorter than the SOME/IP header is NOT a failure: the packet is
     * returned with empty SOME/IP and SD payloads, and the extractor still produces
     * a feature vector from it. */
    if (out.plL4.size() < kSomeipHeaderBytes)
    {
        return Res::FromValue(out);
    }
    else
    {
        /* A full SOME/IP header is present. */
    }

    out.serviceId = ReadBe16(out.plL4, kServiceIdOffset);
    out.methodId  = ReadBe16(out.plL4, kMethodIdOffset);

    const PayloadView body = out.plL4.subspan(kSomeipHeaderBytes, out.plL4.size() - kSomeipHeaderBytes);

    /* Service Discovery is decided by either port or by the reserved service
     * identifier, and the two payloads are mutually exclusive: whichever one is
     * filled, the other stays empty. */
    const bool isSd = ((out.flow.dport == kSdPort) || (out.flow.sport == kSdPort) || (out.serviceId == kSdServiceId));
    if (isSd)
    {
        out.plSd = body;
    }
    else
    {
        out.plSomeip = body;
    }

    return Res::FromValue(out);
}

} // namespace ids
