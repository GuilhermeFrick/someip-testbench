/**
 * @file      Sensor.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     The chain: parse, extract, normalise, infer, decide.
 * @version   1.0
 * @date      2026-11-01
 * @copyright Copyright (c) 2026
 *
 * @details   The sensor implements no stage. It chains them and times them, and the split into
 *            four stages is the same cost decomposition the paper reports, so instrumentation and
 *            publication land on the same boundaries with no translation.
 *
 * @details   ON THE COST OF MEASURING. One frame takes FIVE clock readings, one per stage boundary,
 *            shared between neighbours rather than taken twice. On this chain, which processes a
 *            frame in the order of a microsecond, five readings of a monotonic clock are not
 *            negligible: they are a visible fraction of the total. That is accepted here because
 *            the purpose of this instrumentation is the RELATIVE weight of the stages, which the
 *            overhead shifts uniformly. The absolute per-packet figure comes from the latency
 *            bench, which measures without this decomposition.
 */
#include "src/ids_sensor/Sensor.hpp"

#include <chrono>

namespace ids
{

namespace
{

constexpr double kSecondsToMicroseconds = 1e6; ///< Scenario clock is in seconds; events are stamped in microseconds.

/**
 * @brief Microseconds between two clock readings.
 * @param[in] from Earlier reading.
 * @param[in] to   Later reading.
 * @return The interval in microseconds.
 */
double ElapsedUs(const std::chrono::steady_clock::time_point &from, const std::chrono::steady_clock::time_point &to)
{
    return std::chrono::duration<double, std::micro>(to - from).count();
}

} // namespace

Sensor::Sensor(SomeipParser &parser, FeatureStreamer &streamer, const Normalizer &norm, const IClassifier &clf,
               ISecurityEventDispatcher &sink)
    : m_parser(parser), m_streamer(streamer), m_norm(norm), m_clf(clf), m_sink(sink), m_timing()
{
}

void Sensor::OnFrame(const Frame &frame)
{
    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

    /* ------------------------------------------------------------- 1. parse */
    const ara::core::Result<ParsedPacket> parsed = m_parser.Parse(frame);
    const std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
    m_timing.parseUs += ElapsedUs(t0, t1);

    if (!parsed.HasValue())
    {
        /* A rejected frame costs only the parse stage and is counted apart. Folding
         * it into `packets` would drag the other three averages down in proportion
         * to how much non-SOME/IP traffic the capture happens to carry. */
        ++m_timing.malformed;
        return;
    }
    else
    {
        /* A packet to process. */
    }
    ++m_timing.packets;

    /* --------------------------------------------------------- 2. features */
    const ara::core::Optional<FeatureVec> raw = m_streamer.Step(parsed.Value());
    const std::chrono::steady_clock::time_point t2 = std::chrono::steady_clock::now();
    m_timing.featureUs += ElapsedUs(t1, t2);

    if (!raw.has_value())
    {
        /* The extractor produced no vector. Nothing downstream can run, and the
         * packet was already counted, so the remaining stages simply get no time
         * from it. */
        return;
    }
    else
    {
        /* A feature vector to classify. */
    }

    /* ------------------------------------------- 3. normalise and classify */
    /* Normalisation is counted with the classification, not as a fifth stage. It is
     * sixteen subtractions and sixteen divisions, below the resolution that a clock
     * reading around it would add, and the paper reports four stages. */
    const FeatureVec normalised = m_norm.Apply(raw.value());
    const ProbVec    prob       = m_clf.Predict(normalised);
    const std::chrono::steady_clock::time_point t3 = std::chrono::steady_clock::now();
    m_timing.classifyUs += ElapsedUs(t2, t3);

    /* ------------------------------------------------------------ 4. decide */
    const TrafficClass predicted = ArgMax(prob);

    SecurityEventContext context;
    context.srcIp      = parsed.Value().flow.src;
    context.serviceId  = parsed.Value().serviceId;
    context.methodId   = parsed.Value().methodId;
    context.confidence = prob[static_cast<std::size_t>(predicted)];

    /* The event is stamped with the SCENARIO clock carried by the frame, not with
     * the clock being used to measure. A capture replayed tomorrow has to produce
     * the same event times as it did today, and a wall clock would not. */
    const SecurityEventTimestamp stamp =
        static_cast<SecurityEventTimestamp>(parsed.Value().ts * kSecondsToMicroseconds);

    if (m_sink.Dispatch(predicted, context, stamp))
    {
        ++m_timing.events;
    }
    else
    {
        /* Benign traffic, or a class that raises no event. */
    }

    const std::chrono::steady_clock::time_point t4 = std::chrono::steady_clock::now();
    m_timing.decideUs += ElapsedUs(t3, t4);
}

void Sensor::Run(IPacketSource &source)
{
    for (ara::core::Optional<Frame> frame = source.Next(); frame.has_value(); frame = source.Next())
    {
        OnFrame(frame.value());
    }
}

} // namespace ids
