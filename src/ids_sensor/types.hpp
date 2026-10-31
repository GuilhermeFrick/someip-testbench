/**
 * @file      types.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Shared types of the ids_sensor component: feature and probability vectors, traffic classes, byte views and the parsed packet.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   Design constraints that hold for every file in this directory:
 *            - no allocation on the hot path: fixed sizes, buffers reserved at construction time;
 *            - payloads are non-owning VIEWS into the frame buffer, never copies;
 *            - AUTOSAR C++14 applies to the ECU path, not to this bench component, which targets
 *              C++17 as required by the C++ code standard.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

/** @defgroup IdsSensor IDS Sensor
 *  @brief Packet-level intrusion detection sensor for SOME/IP traffic.
 */

/** @addtogroup IdsSensorTypes Shared types
 *  @ingroup IdsSensor
 *  @{
 */

namespace ids
{

static constexpr std::size_t kNumFeatures = 16U; ///< Number of features produced by the extractor.
static constexpr std::size_t kNumClasses  = 5U;  ///< Number of traffic classes the classifier discriminates.
static constexpr std::size_t kByteValues  = 256U; ///< Number of distinct byte values a byte model tabulates.

using FeatureVec = std::array<float, kNumFeatures>; ///< One raw or normalised feature vector.
using ProbVec    = std::array<float, kNumClasses>;  ///< Per-class probabilities produced by a classifier.

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
 * @brief Non-owning view over bytes of the current frame.
 * @warning Does not outlive the frame it points into.
 */
struct Bytes
{
    const std::uint8_t *data = nullptr; ///< First byte of the view, or nullptr when empty.
    std::size_t         len  = 0U;      ///< Length of the view in bytes.
};

/**
 * @brief Raw frame, as delivered by a packet source.
 */
struct Frame
{
    const std::uint8_t *data = nullptr; ///< First byte of the frame.
    std::size_t         len  = 0U;      ///< Frame length in bytes.
    double              ts   = 0.0;     ///< Timestamp in seconds, from the single scenario clock.
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
    [[nodiscard]] bool operator==(const FlowKey &other) const
    {
        return ((src == other.src) && (dst == other.dst) && (sport == other.sport) && (dport == other.dport) && (proto == other.proto));
    }
};

/**
 * @brief FNV-1a hash of a FlowKey, for use with unordered containers.
 */
struct FlowKeyHash
{
    static constexpr std::uint64_t FNV_PRIME = 1099511628211ULL; ///< FNV-1a 64-bit multiplier.
    static constexpr std::uint64_t PORT_SHIFT = 16ULL;           ///< Bit shift that packs both ports into one word.

    /**
     * @brief Hash a flow key.
     * @param[in] key Key to hash.
     * @return Hash value, truncated to the platform word size.
     */
    [[nodiscard]] std::size_t operator()(const FlowKey &key) const
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
 * @details Stateless: depends only on the frame it was decoded from.
 */
struct ParsedPacket
{
    bool          valid = false; ///< false when the frame is not SOME/IP or is malformed.
    double        ts    = 0.0;   ///< Timestamp copied from the frame, in seconds.
    FlowKey       flow;          ///< Flow the packet belongs to.
    std::uint16_t ipLen = 0U;    ///< Total length taken from the IP header.
    Bytes         plL4;          ///< Transport payload.
    Bytes         plSomeip;      ///< SOME/IP payload, header excluded.
    Bytes         plSd;          ///< Service Discovery payload, when present.
};

} // namespace ids

/** @}*/ // End of IdsSensorTypes
