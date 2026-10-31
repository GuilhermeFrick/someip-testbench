/**
 * @file      types.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Shared types of the ids_sensor component: payload views, feature and probability
 *            vectors, traffic classes, flow key and the parsed packet.
 * @version   2.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   Design constraints that hold for every file in this directory:
 *            - the component follows the AUTOSAR Adaptive Platform pattern, in C++14, per the
 *              AUTOSAR C++14 guidelines. The platform types it uses live in src/ara, as a bench
 *              subset of the specification;
 *            - OUTPUT PARAMETERS ARE NOT USED, per rule A8-4-8. An operation that may fail returns
 *              ara::core::Result; one whose value may legitimately be absent returns
 *              ara::core::Optional; one that cannot fail returns its value directly. This is the
 *              one point where the pattern departs from the C++ code standard, whose section 7
 *              recommends overloading on an output parameter instead;
 *            - no allocation on the hot path: fixed sizes, buffers reserved at construction time;
 *            - payloads are non-owning VIEWS into the frame buffer, never copies.
 *
 * @details   On discarded return values: the C++ code standard asks for @c [[nodiscard]], which is
 *            a C++17 attribute. Wrapping it in a macro would breach rule A16-0-1, which restricts
 *            the pre-processor to include guards and file inclusion, and it is unnecessary anyway,
 *            because rule A0-1-2 already requires that the value returned by a non-void function
 *            be used. That rule is enforced by static analysis rather than by an attribute, and
 *            returning values instead of writing through output parameters is what makes it
 *            meaningful here.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "src/ara/core/Span.hpp"

/** @defgroup IdsSensor IDS Sensor
 *  @brief Packet-level intrusion detection sensor for SOME/IP traffic.
 */

/** @addtogroup IdsSensorTypes Shared types
 *  @ingroup IdsSensor
 *  @{
 */

namespace ids
{

static constexpr std::size_t kNumFeatures = 16U;  ///< Number of features produced by the extractor.
static constexpr std::size_t kNumClasses  = 5U;   ///< Number of traffic classes the classifier discriminates.
static constexpr std::size_t kByteValues  = 256U; ///< Number of distinct byte values a byte model tabulates.

using PayloadView = ara::core::Span<const std::uint8_t>; ///< Non-owning view over bytes of the current frame.
using FeatureVec  = std::array<float, kNumFeatures>;     ///< One raw or normalised feature vector.
using ProbVec     = std::array<float, kNumClasses>;      ///< Per-class probabilities produced by a classifier.

/**
 * @brief Traffic classes, in the SAME order used during training.
 * @details The order is a contract: the index returned by the classifier indexes this
 *          enumeration directly, so reordering it silently remaps every prediction.
 */
enum class TrafficClass : std::uint8_t
{
    Normal     = 0U, ///< Benign traffic.
    Dos        = 1U, ///< Denial of service.
    Fuzzy      = 2U, ///< Malformed or randomised payloads.
    MitmSingle = 3U, ///< Man in the middle from a single source.
    MitmMulti  = 4U, ///< Man in the middle from multiple sources.
};

/**
 * @brief Raw frame, as delivered by a packet source.
 * @warning The view does not outlive the buffer the source owns.
 */
struct Frame
{
    PayloadView bytes;     ///< View over the whole frame.
    double      ts = 0.0;  ///< Timestamp in seconds, from the single scenario clock.
};

/**
 * @brief Compact flow key.
 * @details Integers rather than strings, so comparison and hashing are cheap and allocation free.
 *          The Python side uses a tuple of strings, which would be costly here.
 */
struct FlowKey
{
    std::uint32_t src   = 0U; ///< Source IPv4 address, host byte order.
    std::uint32_t dst   = 0U; ///< Destination IPv4 address, host byte order.
    std::uint16_t sport = 0U; ///< Source transport port.
    std::uint16_t dport = 0U; ///< Destination transport port.
    std::uint8_t  proto = 0U; ///< IP protocol number.

    /**
     * @brief Compare two flow keys field by field.
     * @param[in] other Key to compare against.
     * @return Whether both keys denote the same flow.
     * @retval true  Every field matches.
     * @retval false At least one field differs.
     */
    bool operator==(const FlowKey &other) const
    {
        return ((src == other.src) && (dst == other.dst) && (sport == other.sport) && (dport == other.dport) && (proto == other.proto));
    }
};

/**
 * @brief FNV-1a hash of a FlowKey, for use with unordered containers.
 */
struct FlowKeyHash
{
    static constexpr std::uint64_t FNV_PRIME  = 1099511628211ULL; ///< FNV-1a 64-bit multiplier.
    static constexpr std::uint64_t PORT_SHIFT = 16ULL;            ///< Bit shift that packs both ports into one word.

    /**
     * @brief Hash a flow key.
     * @param[in] key Key to hash.
     * @return Hash value, truncated to the platform word size.
     */
    std::size_t operator()(const FlowKey &key) const
    {
        std::uint64_t hash = key.src;
        hash               = ((hash * FNV_PRIME) ^ key.dst);
        hash               = ((hash * FNV_PRIME) ^ ((static_cast<std::uint64_t>(key.sport) << PORT_SHIFT) | key.dport));
        hash               = ((hash * FNV_PRIME) ^ key.proto);
        return static_cast<std::size_t>(hash);
    }
};

/**
 * @brief Result of layered parsing (Ethernet to IP to TCP/UDP to SOME/IP to SD).
 * @details Stateless: depends only on the frame it was decoded from. A packet that could not be
 *          decoded is never represented here; the parser reports ::ids::IdsSensorErrc::MalformedPacket
 *          instead, so there is no half-filled instance to guard against.
 */
struct ParsedPacket
{
    double        ts    = 0.0; ///< Timestamp copied from the frame, in seconds.
    FlowKey       flow;        ///< Flow the packet belongs to.
    std::uint16_t ipLen = 0U;  ///< Total length taken from the IP header.
    std::uint16_t serviceId = 0U; ///< SOME/IP service identifier.
    std::uint16_t methodId  = 0U; ///< SOME/IP method or event identifier.
    PayloadView   plL4;        ///< Transport payload.
    PayloadView   plSomeip;    ///< SOME/IP payload, header excluded.
    PayloadView   plSd;        ///< Service Discovery payload, empty when the packet is not SD.
};

} // namespace ids

/** @}*/ // End of IdsSensorTypes
