/**
 * @file      gnss_ecu.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Position sensor node: offers GnssService and publishes Position cyclically.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   First node written against libecu, and the one that closes Phase 0: it takes the
 *            place of the vsomeip @c notify-sample in the very scenario that platform/sd-smoke
 *            had already validated.
 *
 * @details   There is not a single hand-written identifier here. Service, instance, event and
 *            eventgroup all come from the generated build/cfg/CommMatrix.hpp, which is produced
 *            from platform/comm_matrix.yaml. Changing an id in the matrix changes the stack
 *            configuration, the PCAP labelling and this code at once, which is the whole reason
 *            the matrix exists.
 */
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

#include "build/cfg/CommMatrix.hpp"
#include "src/libecu/Skeleton.hpp"

namespace
{

/// Cleared by the signal handler so the main loop can leave cleanly.
std::atomic<bool> g_stop(false);

/**
 * @brief Handler for SIGINT and SIGTERM.
 * @param[in] signalNumber Signal received; not used.
 * @return None.
 */
void OnSignal(int signalNumber)
{
    static_cast<void>(signalNumber);
    g_stop.store(true);
}

constexpr std::chrono::milliseconds kPublishPeriod{1000};      ///< Period of the publishing loop.
constexpr std::chrono::milliseconds kRegistrationWait{10000};  ///< How long to wait for the routing manager.

constexpr std::size_t  kPayloadBytes = 8U;           ///< Size of the published payload.
constexpr std::uint8_t kMarker0      = 0xDEU;        ///< First byte of the trailing marker.
constexpr std::uint8_t kMarker1      = 0xADU;        ///< Second byte of the trailing marker.
constexpr std::uint8_t kMarker2      = 0xBEU;        ///< Third byte of the trailing marker.
constexpr std::uint8_t kMarker3      = 0xEFU;        ///< Fourth byte of the trailing marker.

} // namespace

int main()
{
    namespace mtx = ids::matrix;

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    ids::ecu::SkeletonConfig cfg;
    cfg.appName  = mtx::GnssEcu::AppName();
    cfg.service  = mtx::GnssService::Id();
    cfg.instance = mtx::GnssService::Instance();

    std::printf("[gnss_ecu] service 0x%04X instance 0x%04X, app \"%s\"\n", static_cast<unsigned>(cfg.service),
                static_cast<unsigned>(cfg.instance), cfg.appName.c_str());
    std::fflush(stdout);

    ara::core::Result<std::unique_ptr<ids::ecu::Skeleton>> created = ids::ecu::Skeleton::Create(cfg);
    if (!created.HasValue())
    {
        std::printf("[gnss_ecu] ERROR creating the skeleton: %s\n", created.Error().Message());
        return 1;
    }
    else
    {
        /* Created: carry on. */
    }
    ids::ecu::Skeleton &skeleton = *created.Value();

    /* Position belongs to two eventgroups in the matrix. Both are declared, so a
     * subscriber may pick either one. */
    const ids::ecu::EventgroupId eventgroups[] = {
        mtx::GnssService::Eventgroups::Group4455(),
        mtx::GnssService::Eventgroups::Group4555(),
    };

    const ara::core::Result<void> declared = skeleton.OfferEvent(mtx::GnssService::Events::Position(),
                                                                ids::ecu::EventgroupIds(eventgroups), true,
                                                                std::chrono::milliseconds::zero());
    if (!declared.HasValue())
    {
        std::printf("[gnss_ecu] ERROR declaring the event: %s\n", declared.Error().Message());
        return 1;
    }
    else
    {
        /* Event declared. */
    }

    const ara::core::Result<void> started = skeleton.Start();
    if (!started.HasValue())
    {
        std::printf("[gnss_ecu] ERROR starting the stack: %s\n", started.Error().Message());
        return 1;
    }
    else
    {
        /* Stack coming up on its own thread. */
    }

    const ara::core::Result<void> registered = skeleton.WaitUntilRegistered(kRegistrationWait);
    if (!registered.HasValue())
    {
        std::printf("[gnss_ecu] ERROR: %s\n", registered.Error().Message());
        return 1;
    }
    else
    {
        std::printf("[gnss_ecu] registered with the routing manager\n");
    }

    const ara::core::Result<void> offered = skeleton.Offer();
    if (!offered.HasValue())
    {
        std::printf("[gnss_ecu] ERROR offering the service: %s\n", offered.Error().Message());
        return 1;
    }
    else
    {
        std::printf("[gnss_ecu] service offered; publishing Position every %lld ms\n",
                    static_cast<long long>(kPublishPeriod.count()));
    }
    std::fflush(stdout);

    /* Eight-byte payload with a big-endian counter up front and a fixed marker
     * after it. The counter lets the subscriber check that notifications arrive
     * in order, and the marker makes the payload recognisable in a capture. */
    std::uint32_t counter = 0U;
    while (!g_stop.load())
    {
        std::array<std::uint8_t, kPayloadBytes> payload = {{0U, 0U, 0U, 0U, kMarker0, kMarker1, kMarker2, kMarker3}};
        payload[0]                                      = static_cast<std::uint8_t>((counter >> 24U) & 0xFFU);
        payload[1]                                      = static_cast<std::uint8_t>((counter >> 16U) & 0xFFU);
        payload[2]                                      = static_cast<std::uint8_t>((counter >> 8U) & 0xFFU);
        payload[3]                                      = static_cast<std::uint8_t>(counter & 0xFFU);

        const ara::core::Result<void> sent =
            skeleton.Send(mtx::GnssService::Events::Position(), ids::ecu::ByteView(payload.data(), payload.size()));
        if (!sent.HasValue())
        {
            std::printf("[gnss_ecu] ERROR sending: %s\n", sent.Error().Message());
        }
        else
        {
            std::printf("[gnss_ecu] Position #%u sent\n", counter);
        }
        std::fflush(stdout);

        ++counter;
        std::this_thread::sleep_for(kPublishPeriod);
    }

    std::printf("[gnss_ecu] shutting down\n");
    return 0;
}
