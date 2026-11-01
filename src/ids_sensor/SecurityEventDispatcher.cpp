/**
 * @file      SecurityEventDispatcher.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Serialisation of the detection context and dispatch through ara::idsm.
 * @version   1.0
 * @date      2026-11-01
 * @copyright Copyright (c) 2026
 *
 * @details   The reporters are built ONE PER REPORTABLE CLASS, at construction, because in
 *            ara::idsm the event identity comes from the ara::core::InstanceSpecifier and not from
 *            a parameter of the reporting call. That is what keeps the detection path free of
 *            specifier resolution; see ../ara/idsm/EventReporter.hpp.
 *
 * @details   ::ids::TrafficClass::Normal raises NO event. It is the common case by a wide margin,
 *            so it is answered first and without touching any reporter.
 */
#include "src/ids_sensor/SecurityEventDispatcher.hpp"

#include <cstring>
#include <utility>

#include "src/ara/core/Optional.hpp"

namespace ids
{

namespace
{

constexpr std::size_t kReportableClasses = (kNumClasses - 1U); ///< Every class but Normal raises an event.
constexpr std::uint32_t kByteMask        = 0xFFU;              ///< One byte out of a wider word.
constexpr std::uint32_t kShift8          = 8U;                 ///< Shift of one byte.
constexpr std::uint32_t kShift16         = 16U;                ///< Shift of two bytes.
constexpr std::uint32_t kShift24         = 24U;                ///< Shift of three bytes.

/**
 * @brief Suffix naming the security event of one class, appended to the port prefix.
 * @param[in] trafficClass Class to name.
 * @return Null-terminated suffix, or @c nullptr for a class that raises no event.
 * @details The strings are part of the meta-model path, so they are a CONTRACT with the
 *          configuration and not a diagnostic. Changing one here without changing the
 *          configuration makes Create() fail, which is the intended outcome: it is better than a
 *          reporter bound to the wrong event definition.
 */
const char *EventSuffix(TrafficClass trafficClass)
{
    const char *suffix = nullptr;
    switch (trafficClass)
    {
        case TrafficClass::Dos:
            suffix = "/DenialOfService";
            break;
        case TrafficClass::Fuzzy:
            suffix = "/MalformedPayload";
            break;
        case TrafficClass::MitmSingle:
            suffix = "/ManInTheMiddleSingle";
            break;
        case TrafficClass::MitmMulti:
            suffix = "/ManInTheMiddleMulti";
            break;
        case TrafficClass::Normal:
        default:
            /* Benign traffic is not an event. Keeps nullptr. */
            break;
    }
    return suffix;
}

/**
 * @brief Index of a reportable class in the reporter array.
 * @param[in] trafficClass Class to index.
 * @return Index in [0, kReportableClasses), or kReportableClasses for a class that raises no event.
 * @details Normal is value zero of the enumeration, so the reportable classes are one less than
 *          their enumerator. Written as a function rather than as arithmetic at each use, so the
 *          dependence on Normal being zero lives in ONE place.
 */
std::size_t ReporterIndex(TrafficClass trafficClass)
{
    std::size_t index = kReportableClasses;
    if (trafficClass != TrafficClass::Normal)
    {
        index = (static_cast<std::size_t>(trafficClass) - 1U);
    }
    else
    {
        /* No reporter for benign traffic. */
    }
    return index;
}

/**
 * @brief Write a 32-bit value in network byte order.
 * @param[in] value Value to encode.
 * @param[in] into  Buffer to write into.
 * @param[in] at    Offset of the first byte.
 * @return None.
 * @details Shifts rather than a cast over the object representation, so the layout of the
 *          artefact does not depend on the byte order of whoever compiled this.
 */
void PutBe32(std::uint32_t value, SecurityEventContext::Buffer &into, std::size_t at)
{
    into[at]        = static_cast<std::uint8_t>((value >> kShift24) & kByteMask);
    into[at + 1U]   = static_cast<std::uint8_t>((value >> kShift16) & kByteMask);
    into[at + 2U]   = static_cast<std::uint8_t>((value >> kShift8) & kByteMask);
    into[at + 3U]   = static_cast<std::uint8_t>(value & kByteMask);
}

/**
 * @brief Write a 16-bit value in network byte order.
 * @param[in] value Value to encode.
 * @param[in] into  Buffer to write into.
 * @param[in] at    Offset of the first byte.
 * @return None.
 */
void PutBe16(std::uint16_t value, SecurityEventContext::Buffer &into, std::size_t at)
{
    into[at]      = static_cast<std::uint8_t>((static_cast<std::uint32_t>(value) >> kShift8) & kByteMask);
    into[at + 1U] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(value) & kByteMask);
}

/**
 * @brief Reinterpret the bits of a float as an unsigned word.
 * @param[in] value Value whose bits are wanted.
 * @return The IEEE-754 single-precision bit pattern.
 * @details Through a copy into an unsigned word, not through a pointer cast, which would breach
 *          the strict aliasing rule and is undefined behaviour however common it is.
 */
std::uint32_t FloatBits(float value)
{
    std::uint32_t bits = 0U;
    static_assert(sizeof(bits) == sizeof(value), "float is not 32 bits on this platform");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

} // namespace

SecurityEventContext::Buffer SecurityEventContext::Serialize() const
{
    Buffer out = Buffer();
    PutBe32(srcIp, out, 0U);
    PutBe16(serviceId, out, 4U);
    PutBe16(methodId, out, 6U);
    /* The confidence travels as its IEEE-754 bit pattern, in network byte order.
     * The meta-model declares the field as four opaque bytes, so whoever reads it
     * has to agree on this and nothing else; a decimal text form would be larger
     * and would lose the last digits. */
    PutBe32(FloatBits(confidence), out, 8U);
    return out;
}

/**
 * @brief The reporters, one per reportable class.
 */
struct IdsmDispatcher::Impl
{
    /* ara::idsm::EventReporter is move-only and has no default constructor, so the
     * reporters live in Optionals and are emplaced as each specifier resolves.
     * An array of reporters would require a default-constructible reporter, which
     * would mean a reporter bound to nothing. */
    ara::core::Optional<ara::idsm::EventReporter> reporters[kReportableClasses];
};

ara::core::Result<std::unique_ptr<IdsmDispatcher>> IdsmDispatcher::Create(const std::string &portPrefix)
{
    using Res = ara::core::Result<std::unique_ptr<IdsmDispatcher>>;

    std::unique_ptr<Impl> impl(new Impl());

    /* Every reporter is resolved HERE, up front. A specifier that does not resolve
     * is a configuration mistake, and it is better to refuse to start than to run a
     * sensor whose detections of one class go nowhere. */
    for (std::size_t i = 0U; i < kReportableClasses; ++i)
    {
        const TrafficClass trafficClass = static_cast<TrafficClass>(i + 1U);
        const char *const  suffix       = EventSuffix(trafficClass);
        if (suffix == nullptr)
        {
            /* Unreachable while Normal is the only class without an event, and the
             * loop starts past it. Guarded because the cost is one comparison at
             * construction and the alternative is a null dereference. */
            return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::InvalidConfiguration));
        }

        ara::core::Result<ara::core::InstanceSpecifier> specifier =
            ara::core::InstanceSpecifier::Create(portPrefix + suffix);
        if (!specifier.HasValue())
        {
            return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::InvalidConfiguration));
        }

        ara::core::Result<ara::idsm::EventReporter> reporter = ara::idsm::EventReporter::Create(specifier.Value());
        if (!reporter.HasValue())
        {
            return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::InvalidConfiguration));
        }

        /* std::move on the VALUE, not on the Result: ara::core::Result::Value() has
         * no rvalue overload, so moving the Result would still hand back an lvalue
         * reference and the move-only reporter would fail to copy. */
        impl->reporters[i] = ara::core::Optional<ara::idsm::EventReporter>(std::move(reporter.Value()));
    }

    return Res::FromValue(std::unique_ptr<IdsmDispatcher>(new IdsmDispatcher(std::move(impl))));
}

IdsmDispatcher::IdsmDispatcher(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl))
{
}

IdsmDispatcher::~IdsmDispatcher() = default;

bool IdsmDispatcher::Dispatch(TrafficClass trafficClass, const SecurityEventContext &context,
                              SecurityEventTimestamp timestamp)
{
    /* Benign traffic first and cheapest: it is the overwhelming majority of packets
     * and it must not pay for anything below. */
    if (trafficClass == TrafficClass::Normal)
    {
        return false;
    }
    else
    {
        /* Something to report. */
    }

    const std::size_t index = ReporterIndex(trafficClass);
    if ((index >= kReportableClasses) || !m_impl->reporters[index].has_value())
    {
        /* A class outside the enumeration, or a reporter that Create() never filled.
         * Create() cannot return with an empty reporter, so this is a guard against a
         * future class added to the enumeration and forgotten here. */
        return false;
    }
    else
    {
        /* The reporter for this class exists. */
    }

    const SecurityEventContext::Buffer bytes = context.Serialize();
    m_impl->reporters[index].value().ReportEvent(ara::idsm::ContextDataType(bytes.data(), bytes.size()), timestamp);
    return true;
}

} // namespace ids
