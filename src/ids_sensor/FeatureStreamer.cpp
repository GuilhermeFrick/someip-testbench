/**
 * @file      FeatureStreamer.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     The 16 features, packet by packet, with the sliding-window state.
 * @version   2.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   Every ordering decision here mirrors @c FeatureStreamer.step() of
 *            @c src/extract_ext.py in the reproduction repository. Where the reference looks
 *            arbitrary it was still followed, because the model was trained on what that code
 *            produces, not on what the paper describes.
 *
 * @details   The two windows are ::ids::util::SlidingWindow. The first version of this file wrote
 *            the window behaviour inline twice, with its own trim loop each time; the component
 *            replaced both. Its Push() returning the EVICTED item is what lets the diversity
 *            window keep its distinct count in constant time without inspecting itself.
 *
 * @details   The windows hold a 64-bit HASH of each payload rather than its bytes. They are only
 *            ever used for equality and for counting distinct values, so a hash answers both
 *            questions, cuts the per-entry cost from up to 1400 bytes to 8, and makes the distinct
 *            count maintainable in constant time. The cost is that a 64-bit collision would make
 *            this extractor disagree with the reference; over a 1000-entry window that probability
 *            is around 3e-14, and the parity test is what would surface it.
 */
#include "src/ids_sensor/FeatureStreamer.hpp"

#include <deque>
#include <unordered_map>
#include <vector>

#include "src/util/SlidingWindow.hpp"

namespace ids
{

namespace
{

/// FNV-1a 64-bit offset basis.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;

/// FNV-1a 64-bit prime.
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

/// Window of payload hashes.
using HashWindow = util::SlidingWindow<std::uint64_t>;

/**
 * @brief FNV-1a hash of a payload, used as its identity inside the windows.
 * @param[in] payload Payload to hash.
 * @return Hash value; an empty payload hashes to the offset basis.
 */
std::uint64_t HashPayload(const PayloadView &payload)
{
    std::uint64_t hash = kFnvOffset;
    for (std::size_t i = 0U; i < payload.size(); ++i)
    {
        hash = ((hash ^ static_cast<std::uint64_t>(payload[i])) * kFnvPrime);
    }
    return hash;
}

/**
 * @brief State kept for one flow: the previous packet, and the repetition window.
 * @details The payloads are COPIED, not referenced. A ::ids::PayloadView points into the frame
 *          buffer, which the packet source reuses on the next call, so keeping the view would
 *          compare against whatever arrived later.
 */
struct FlowState
{
    /**
     * @brief Build the state with an empty window of the configured size.
     * @param[in] prototype Empty window of the right capacity, copied in.
     * @return None.
     */
    explicit FlowState(const HashWindow &prototype) : recent(prototype)
    {
    }

    double                    ts    = 0.0; ///< Timestamp of the previous packet.
    std::uint16_t             ipLen = 0U;  ///< IP total length of the previous packet.
    std::vector<std::uint8_t> l4;          ///< Previous transport payload.
    std::vector<std::uint8_t> someip;      ///< Previous SOME/IP payload.
    std::vector<std::uint8_t> sd;          ///< Previous Service Discovery payload.
    HashWindow                recent;      ///< Hashes of the last payloads of this flow.
};

/**
 * @brief Window of payload hashes seen from one source, with a constant-time distinct count.
 */
struct SourceState
{
    /**
     * @brief Build the state with an empty window of the configured size.
     * @param[in] prototype Empty window of the right capacity, copied in.
     * @return None.
     */
    explicit SourceState(const HashWindow &prototype) : window(prototype)
    {
    }

    HashWindow                                     window;        ///< Hashes currently in the window.
    std::unordered_map<std::uint64_t, std::size_t> counts;         ///< How many times each hash is in the window.
    std::size_t                                    distinct = 0U; ///< Size of the key set of counts.
};

/**
 * @brief Copy a payload view into an owned buffer.
 * @param[in]     payload Payload to copy.
 * @param[in,out] into    Buffer that receives the bytes; its capacity is kept.
 * @return None.
 */
void CopyInto(const PayloadView &payload, std::vector<std::uint8_t> &into)
{
    into.assign(payload.data(), payload.data() + payload.size());
}

/**
 * @brief View over an owned buffer, for comparing against the current payload.
 * @param[in] buffer Buffer to view.
 * @return View over the buffer.
 */
PayloadView ViewOf(const std::vector<std::uint8_t> &buffer)
{
    return PayloadView(buffer.data(), buffer.size());
}

} // namespace

/**
 * @brief Per-flow and per-source window state of the extractor.
 */
struct FeatureStreamer::Impl
{
    /**
     * @brief Build with the two empty window prototypes.
     * @param[in] recent Empty repetition window, of the configured size.
     * @param[in] source Empty diversity window, of the configured size.
     * @return None.
     */
    Impl(const HashWindow &recent, const HashWindow &source) : recentPrototype(recent), sourcePrototype(source)
    {
    }

    const ByteModels *models = nullptr; ///< Byte models; owned elsewhere and must outlive us.
    StreamerConfig    cfg;              ///< Window sizes and caps.

    /// Empty windows of the right capacity, copied into each new flow and source. Built once, so
    /// no per-flow path has to validate a capacity or handle a failure that cannot happen.
    HashWindow recentPrototype;
    HashWindow sourcePrototype;

    std::unordered_map<FlowKey, FlowState, FlowKeyHash> flows;   ///< State per flow.
    std::deque<FlowKey>                                 flowAge; ///< Flow keys in insertion order, for eviction.

    std::unordered_map<std::uint32_t, SourceState> sources;   ///< State per source address.
    std::deque<std::uint32_t>                      sourceAge; ///< Source addresses in insertion order.

    std::uint64_t flowEvictions   = 0U; ///< Flows dropped because the cap was reached.
    std::uint64_t sourceEvictions = 0U; ///< Sources dropped because the cap was reached.
};

FeatureStreamer::FeatureStreamer(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl))
{
}

FeatureStreamer::~FeatureStreamer() = default;

ara::core::Result<std::unique_ptr<FeatureStreamer>> FeatureStreamer::Create(const ByteModels &models, const StreamerConfig &cfg)
{
    using Res = ara::core::Result<std::unique_ptr<FeatureStreamer>>;

    if ((cfg.maxFlows == 0U) || (cfg.maxSources == 0U))
    {
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::InvalidConfiguration));
    }
    else
    {
        /* Caps are usable. */
    }

    /* The window sizes are validated by the component itself, which refuses a
     * capacity of zero. Translating its error into ours keeps a single error
     * domain on this interface. */
    const ara::core::Result<HashWindow> recent = HashWindow::Create(cfg.recentPayloadWindow);
    const ara::core::Result<HashWindow> source = HashWindow::Create(cfg.sourcePayloadWindow);
    if ((!recent.HasValue()) || (!source.HasValue()))
    {
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::InvalidConfiguration));
    }
    else
    {
        /* Both window sizes are usable. */
    }

    std::unique_ptr<Impl> impl(new Impl(recent.Value(), source.Value()));
    impl->models = &models;
    impl->cfg    = cfg;

    /* Reserved up front so that Step() does not rehash while running. */
    impl->flows.reserve(cfg.maxFlows);
    impl->sources.reserve(cfg.maxSources);

    return Res::FromValue(std::unique_ptr<FeatureStreamer>(new FeatureStreamer(std::move(impl))));
}

ara::core::Optional<FeatureVec> FeatureStreamer::Step(const ParsedPacket &packet)
{
    Impl              &impl   = *m_impl;
    const FlowKey     &key    = packet.flow;
    const PayloadView &someip = packet.plSomeip;
    const PayloadView &l4     = packet.plL4;
    const PayloadView &sd     = packet.plSd;

    /* ---------------------------------------------------------------- flow slot */
    std::unordered_map<FlowKey, FlowState, FlowKeyHash>::iterator it = impl.flows.find(key);
    const bool                                                    isNewFlow = (it == impl.flows.end());
    if (isNewFlow)
    {
        if (impl.flows.size() >= impl.cfg.maxFlows)
        {
            /* Evicts the oldest flow. The reference has NO cap, so from here on this
             * extractor and the reference may disagree; FlowEvictions() is what makes
             * that visible instead of looking like an arithmetic bug. */
            const FlowKey oldest = impl.flowAge.front();
            impl.flowAge.pop_front();
            static_cast<void>(impl.flows.erase(oldest));
            ++impl.flowEvictions;
        }
        else
        {
            /* Room left. */
        }
        it = impl.flows.emplace(key, FlowState(impl.recentPrototype)).first;
        impl.flowAge.push_back(key);
        it->second.l4.reserve(impl.cfg.reservedPayloadBytes);
        it->second.someip.reserve(impl.cfg.reservedPayloadBytes);
        it->second.sd.reserve(impl.cfg.reservedPayloadBytes);
    }
    else
    {
        /* Flow already known. */
    }
    FlowState &flow = it->second;

    FeatureVec out{};

    /* ------------------------------------------- timing and change, 12 baseline */
    if (isNewFlow)
    {
        /* First packet of a flow has nothing to compare against, so every
         * difference is zero. Not the content features below, which depend only
         * on the packet itself. */
        out[0]  = 0.0F; /* ip_time_interval */
        out[7]  = 0.0F; /* someip_change   */
        out[8]  = 0.0F; /* sd_change       */
        out[9]  = 0.0F; /* l4_change       */
        out[10] = 0.0F; /* ip_len_change   */
        out[11] = 0.0F; /* l4_len_change   */
    }
    else
    {
        out[0]  = static_cast<float>(packet.ts - flow.ts);
        out[10] = static_cast<float>(static_cast<std::int32_t>(packet.ipLen) - static_cast<std::int32_t>(flow.ipLen));
        out[11] = static_cast<float>(static_cast<std::int64_t>(l4.size()) - static_cast<std::int64_t>(flow.l4.size()));

        /* l4_change has no emptiness guard in the reference: it is always computed. */
        out[9] = static_cast<float>(HammingDistance(l4, ViewOf(flow.l4)));

        /* someip_change and sd_change are guarded, and the guard is an OR, not an
         * AND: the distance is computed when EITHER side has bytes, and is zero
         * only when both are empty. HammingDistance() already returns zero in that
         * case, so the guard is kept here only to mirror the reference plainly. */
        const bool someipAny = ((someip.size() != 0U) || (!flow.someip.empty()));
        out[7]               = someipAny ? static_cast<float>(HammingDistance(someip, ViewOf(flow.someip))) : 0.0F;

        const bool sdAny = ((sd.size() != 0U) || (!flow.sd.empty()));
        out[8]           = sdAny ? static_cast<float>(HammingDistance(sd, ViewOf(flow.sd))) : 0.0F;
    }

    /* ----------------------------------------------- content, 12 baseline (rest) */
    /* Score() returns zero on both for an empty payload, which is what the
     * reference's emptiness guards amount to. */
    const PayloadScores someipScores = impl.models->Someip().Score(someip);
    const PayloadScores sdScores     = impl.models->Sd().Score(sd);
    const PayloadScores l4Scores     = impl.models->L4().Score(l4);

    out[1] = someipScores.logLik;
    out[2] = sdScores.logLik;
    out[3] = l4Scores.logLik;
    out[4] = someipScores.crossEntropy;
    out[5] = sdScores.crossEntropy;
    out[6] = l4Scores.crossEntropy;

    /* ------------------------------------------ feature 12: payload repetition */
    const std::uint64_t someipHash = HashPayload(someip);

    /* Computed BEFORE the current payload joins the window, so the window holds
     * only earlier packets. The reference also requires the current payload to be
     * non-empty, otherwise the rate is zero. */
    if ((!flow.recent.Empty()) && (someip.size() != 0U))
    {
        std::size_t matches = 0U;
        for (std::size_t i = 0U; i < flow.recent.Size(); ++i)
        {
            if (flow.recent.At(i) == someipHash)
            {
                ++matches;
            }
            else
            {
                /* Different payload. */
            }
        }
        out[12] = static_cast<float>(static_cast<double>(matches) / static_cast<double>(flow.recent.Size()));
    }
    else
    {
        out[12] = 0.0F;
    }

    /* Pushed UNCONDITIONALLY, empty payloads included. That asymmetry against the
     * diversity window below is in the reference, and it matters: an empty payload
     * still enlarges the denominator of later packets of this flow. Nothing is done
     * with the evicted hash here, because this window keeps no aggregate. */
    static_cast<void>(flow.recent.Push(someipHash));

    /* ------------------------------------------------ features 13 and 14: size */
    out[13] = static_cast<float>(someip.size());
    out[14] = static_cast<float>(l4.size());

    /* ------------------------------------------- feature 15: source diversity */
    std::unordered_map<std::uint32_t, SourceState>::iterator sit = impl.sources.find(key.src);
    if (sit == impl.sources.end())
    {
        if (impl.sources.size() >= impl.cfg.maxSources)
        {
            const std::uint32_t oldest = impl.sourceAge.front();
            impl.sourceAge.pop_front();
            static_cast<void>(impl.sources.erase(oldest));
            ++impl.sourceEvictions;
        }
        else
        {
            /* Room left. */
        }
        sit = impl.sources.emplace(key.src, SourceState(impl.sourcePrototype)).first;
        impl.sourceAge.push_back(key.src);
        sit->second.counts.reserve(impl.cfg.sourcePayloadWindow);
    }
    else
    {
        /* Source already known. */
    }
    SourceState &source = sit->second;

    /* Pushed only when non-empty, unlike the repetition window above. */
    if (someip.size() != 0U)
    {
        const ara::core::Optional<std::uint64_t> evicted = source.window.Push(someipHash);

        if ((source.counts[someipHash]++) == 0U)
        {
            ++source.distinct;
        }
        else
        {
            /* Hash already present in the window. */
        }

        /* This is why Push() hands the evicted item back: the distinct count can be
         * maintained without walking the window. The reference recomputes
         * len(set(window)) on every packet, which is O(window) and is 42% of the
         * extraction cost. */
        if (evicted.has_value())
        {
            const std::unordered_map<std::uint64_t, std::size_t>::iterator cit = source.counts.find(evicted.value());
            if (cit != source.counts.end())
            {
                --(cit->second);
                if (cit->second == 0U)
                {
                    static_cast<void>(source.counts.erase(cit));
                    --source.distinct;
                }
                else
                {
                    /* Still present through another entry. */
                }
            }
            else
            {
                /* Cannot happen while window and counts agree. */
            }
        }
        else
        {
            /* The window was not full, so nothing left it. */
        }
    }
    else
    {
        /* Empty payload does not enter the diversity window. */
    }

    /* Computed AFTER the push, so the current packet IS counted. A window of a
     * single entry gives zero rather than one, which is the reference's guard. */
    out[15] = (source.window.Size() > 1U)
                  ? static_cast<float>(static_cast<double>(source.distinct) / static_cast<double>(source.window.Size()))
                  : 0.0F;

    /* --------------------------------------------------- remember this packet */
    flow.ts    = packet.ts;
    flow.ipLen = packet.ipLen;
    CopyInto(l4, flow.l4);
    CopyInto(someip, flow.someip);
    CopyInto(sd, flow.sd);

    return ara::core::Optional<FeatureVec>(out);
}

void FeatureStreamer::Reset()
{
    m_impl->flows.clear();
    m_impl->flowAge.clear();
    m_impl->sources.clear();
    m_impl->sourceAge.clear();
    m_impl->flowEvictions   = 0U;
    m_impl->sourceEvictions = 0U;
    m_impl->flows.reserve(m_impl->cfg.maxFlows);
    m_impl->sources.reserve(m_impl->cfg.maxSources);
}

std::uint64_t FeatureStreamer::FlowEvictions() const noexcept
{
    return m_impl->flowEvictions;
}

std::uint64_t FeatureStreamer::SourceEvictions() const noexcept
{
    return m_impl->sourceEvictions;
}

} // namespace ids
