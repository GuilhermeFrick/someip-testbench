/**
 * @file      EventReporter.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Bench binding of the ara::idsm reporter: resolution against configuration, and the
 *            event sink.
 * @version   1.0
 * @date      2026-11-01
 * @copyright Copyright (c) 2026
 *
 * @details   WHAT A REAL PLATFORM DOES, AND WHAT THIS DOES INSTEAD. On an Adaptive Platform the
 *            specifier is resolved through the meta-model to a @c SecurityEventDefinition, and the
 *            reporting call hands the occurrence to the IdsM process, which applies the filter
 *            chain bound by the @c SecurityEventContextMapping. There is no IdsM process on this
 *            bench yet (see @c src/libidsm), so this binding does two things and is explicit about
 *            both:
 *            - RESOLUTION is against a configuration FILE listing the configured event paths, one
 *              per line, named by the environment variable @c ARA_IDSM_CONFIGURATION. A specifier
 *              absent from that list does not resolve, and Create() fails. The configuration is
 *              MANDATORY: with the variable unset, or the file missing, nothing resolves and no
 *              reporter is built;
 *            - REPORTING appends one line per occurrence to the sink named by @c ARA_IDSM_SINK, or
 *              to stderr when that variable is unset.
 *
 * @details   Why configuration is mandatory rather than permissive: a reporter that accepted any
 *            well formed path would make ::ara::idsm::EventReporter::Create() unable to fail, and
 *            its ara::core::Result would be declaring an error that nothing produces. The audit in
 *            @c tests/README.md found six such codes in this project; this is one that was going to
 *            be the seventh. Requiring configuration is also what the platform does, so the stricter
 *            behaviour is the faithful one.
 *
 * @details   NO FILTERING HAPPENS HERE, deliberately. Filtering, aggregation and qualification
 *            belong to the IdsM. A sensor that filtered its own events would be deciding what the
 *            IdsM is configured to decide.
 */
#include "src/ara/idsm/EventReporter.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "src/ara/core/CoreErrorDomain.hpp"

namespace ara
{
namespace idsm
{

namespace
{

constexpr const char *kConfigurationVariable = "ARA_IDSM_CONFIGURATION"; ///< Names the configured-event list.
constexpr const char *kSinkVariable          = "ARA_IDSM_SINK";          ///< Names the file events are appended to.

/**
 * @brief Read the configured event paths.
 * @return The paths listed in the configuration file; empty when there is no usable configuration.
 * @details Read ONCE, on first use, and kept for the life of the process: the configuration of a
 *          running platform does not change underneath it, and re-reading per reporter would make
 *          construction depend on file system timing.
 * @details Blank lines are skipped and a leading @c '#' makes a comment, so the file can be
 *          generated with a header saying where it came from.
 */
const std::vector<std::string> &ConfiguredEvents()
{
    static const std::vector<std::string> events = []() {
        std::vector<std::string> found;
        const char *const        path = std::getenv(kConfigurationVariable);
        if (path == nullptr)
        {
            return found;
        }
        std::ifstream input(path);
        std::string   line;
        while (std::getline(input, line))
        {
            /* Tolerate a carriage return, so a file written on Windows still reads
             * as the same list. Without this the last short name would carry a
             * stray character and nothing would ever resolve, with no clue why. */
            while (!line.empty() && ((line[line.size() - 1U] == '\r') || (line[line.size() - 1U] == ' ')))
            {
                line.erase(line.size() - 1U);
            }
            if (!line.empty() && (line[0] != '#'))
            {
                found.push_back(line);
            }
        }
        return found;
    }();
    return events;
}

/**
 * @brief Whether a path is among the configured events.
 * @param[in] path Meta-model path to look for.
 * @return Whether the configuration lists exactly this path.
 */
bool IsConfigured(const std::string &path)
{
    const std::vector<std::string> &events = ConfiguredEvents();
    bool                            found  = false;
    for (std::size_t i = 0U; (i < events.size()) && !found; ++i)
    {
        found = (events[i] == path);
    }
    return found;
}

constexpr std::size_t kLineBytes = 512U; ///< Fixed room for one sink line, including its terminator.
constexpr std::size_t kHexBytes  = 128U; ///< Fixed room for the context bytes in hexadecimal.

/**
 * @brief Append one line to the event sink.
 * @param[in] line Null-terminated line to append, without its terminator.
 * @return None.
 * @details Opened and closed per occurrence, and flushed by the close. That is the wrong trade for
 *          a hot path and the right one for a bench: an event that was reported but lost in a
 *          buffer when the scenario ended would be indistinguishable from an event never raised.
 *          When the IdsM node exists, this is the function that goes away.
 */
void AppendToSink(const char *line)
{
    const char *const path = std::getenv(kSinkVariable);
    if (path == nullptr)
    {
        static_cast<void>(std::fprintf(stderr, "%s\n", line));
        return;
    }
    std::FILE *const file = std::fopen(path, "ab");
    if (file == nullptr)
    {
        /* The sink is unwritable. Falling back to stderr loses the destination but
         * not the event, which is the lesser loss of the two. */
        static_cast<void>(std::fprintf(stderr, "%s\n", line));
        return;
    }
    static_cast<void>(std::fprintf(file, "%s\n", line));
    static_cast<void>(std::fclose(file));
}

/**
 * @brief Render the context bytes as hexadecimal, into a caller-provided buffer.
 * @param[in]  contextData Opaque context bytes.
 * @param[out] into        Buffer of @c kHexBytes, which receives null-terminated text.
 * @return None.
 * @details Hexadecimal because the bytes are OPAQUE here: their layout belongs to the meta-model,
 *          and this binding must not pretend to know it. Whoever reads the sink decodes them with
 *          the layout its own definition states.
 * @details This is the ONE place in the component with an output parameter, which rule A8-4-8
 *          forbids. The alternative is returning a std::string, and that allocates on the
 *          reporting path, which the interface of ::ara::idsm::EventReporter promises it does not
 *          do. A fixed buffer filled in place keeps the promise that matters; the rule exists to
 *          stop values travelling invisibly, and here the value is the buffer the caller owns.
 * @warning Context longer than @c kHexBytes/2 bytes is TRUNCATED, and the text ends in @c "...".
 *          The bench context is twelve bytes, so this is headroom and not a limit in practice.
 */
void ToHex(const ContextDataType &contextData, char (&into)[kHexBytes])
{
    static const char *const digits = "0123456789abcdef";

    if (contextData.empty())
    {
        into[0] = '-';
        into[1] = '\0';
        return;
    }

    const std::size_t cabem    = ((kHexBytes - 4U) / 2U);
    const bool        truncado = (contextData.size() > cabem);
    const std::size_t quantos  = truncado ? cabem : contextData.size();

    std::size_t at = 0U;
    for (std::size_t i = 0U; i < quantos; ++i)
    {
        into[at] = digits[(contextData[i] >> 4U) & 0x0FU];
        ++at;
        into[at] = digits[contextData[i] & 0x0FU];
        ++at;
    }
    if (truncado)
    {
        into[at] = '.';
        into[at + 1U] = '.';
        into[at + 2U] = '.';
        at += 3U;
    }
    into[at] = '\0';
}

} // namespace

/**
 * @brief Platform binding of one reporter.
 */
struct EventReporter::Impl
{
    std::string eventPath; ///< Meta-model path of the resolved security event.
};

core::Result<EventReporter> EventReporter::Create(const core::InstanceSpecifier &specifier)
{
    using Res = core::Result<EventReporter>;

    if (!IsConfigured(specifier.ToString()))
    {
        /* Either there is no configuration at all, or this event is not in it. Both
         * are configuration mistakes, and both are better refused here than turned
         * into a reporter whose events go nowhere. */
        return Res::FromError(core::ErrorCode(core::CoreErrc::kInvalidMetaModelPath));
    }
    else
    {
        /* The event is configured. */
    }

    std::unique_ptr<Impl> impl(new Impl());
    impl->eventPath = specifier.ToString();
    return Res::FromValue(EventReporter(std::move(impl)));
}

EventReporter::EventReporter(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl))
{
}

EventReporter::~EventReporter() = default;

EventReporter::EventReporter(EventReporter &&other) noexcept : m_impl(std::move(other.m_impl))
{
}

EventReporter &EventReporter::operator=(EventReporter &&other) noexcept
{
    if (this != &other)
    {
        m_impl = std::move(other.m_impl);
    }
    else
    {
        /* Self assignment leaves the binding alone. */
    }
    return *this;
}

void EventReporter::ReportEvent(CountType count) noexcept
{
    ReportEvent(ContextDataType(), count);
}

void EventReporter::ReportEvent(const ContextDataType &contextData, CountType count) noexcept
{
    char hex[kHexBytes];
    ToHex(contextData, hex);
    char linha[kLineBytes];
    /* No timestamp given: the platform would stamp it. This binding records that it
     * was not given, rather than inventing a clock reading that the caller did not
     * ask for and that would not share the scenario clock. */
    static_cast<void>(std::snprintf(linha, sizeof(linha), "security_event path=%s ts=- count=%u context=%s",
                                    m_impl->eventPath.c_str(), static_cast<unsigned>(count), hex));
    AppendToSink(linha);
}

void EventReporter::ReportEvent(const ContextDataType &contextData, TimestampType timestamp, CountType count) noexcept
{
    char hex[kHexBytes];
    ToHex(contextData, hex);
    char linha[kLineBytes];
    static_cast<void>(std::snprintf(linha, sizeof(linha), "security_event path=%s ts=%llu count=%u context=%s",
                                    m_impl->eventPath.c_str(), static_cast<unsigned long long>(timestamp),
                                    static_cast<unsigned>(count), hex));
    AppendToSink(linha);
}

} // namespace idsm
} // namespace ara
