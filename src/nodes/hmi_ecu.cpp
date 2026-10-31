/**
 * @file      hmi_ecu.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Consumer node: discovers GnssService and receives its Position notifications.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   Counterpart of gnss_ecu, taking the place of the vsomeip @c subscribe-sample.
 *            Together they close the Phase 0 acceptance criterion: two nodes of OUR OWN, built
 *            from the communication matrix, exchanging an event that the dissector decodes.
 *
 * @details   The two waits are different things, and telling them apart is what diagnoses the
 *            usual failure. WaitUntilRegistered() says OUR stack came up; WaitUntilAvailable()
 *            says Service Discovery FOUND the provider. Registering and then never becoming
 *            available is the signature of the missing multicast route on one of the two nodes.
 */
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

#include "build/cfg/CommMatrix.hpp"
#include "src/libecu/Proxy.hpp"

namespace
{

/// Cleared by the signal handler so the main loop can leave cleanly.
std::atomic<bool> g_stop(false);

/// How many notifications arrived. Written from the stack thread, read from main.
std::atomic<std::uint32_t> g_received(0U);

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

constexpr std::chrono::milliseconds kRegistrationWait{10000}; ///< How long to wait for the routing manager.
constexpr std::chrono::milliseconds kAvailabilityWait{20000}; ///< How long to wait for Service Discovery.
constexpr std::chrono::milliseconds kIdlePoll{200};           ///< Idle period of the main loop.

constexpr std::size_t kCounterBytes = 4U; ///< Bytes of the big-endian counter at the head of the payload.

} // namespace

int main()
{
    namespace mtx = ids::matrix;

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    ids::ecu::ProxyConfig cfg;
    cfg.appName  = mtx::HmiEcu::AppName();
    cfg.service  = mtx::GnssService::Id();
    cfg.instance = mtx::GnssService::Instance();

    std::printf("[hmi_ecu] looking for service 0x%04X instance 0x%04X, app \"%s\"\n", static_cast<unsigned>(cfg.service),
                static_cast<unsigned>(cfg.instance), cfg.appName.c_str());
    std::fflush(stdout);

    ara::core::Result<std::unique_ptr<ids::ecu::Proxy>> created = ids::ecu::Proxy::Create(cfg);
    if (!created.HasValue())
    {
        std::printf("[hmi_ecu] ERROR creating the proxy: %s\n", created.Error().Message());
        return 1;
    }
    else
    {
        /* Created: carry on. */
    }
    ids::ecu::Proxy &proxy = *created.Value();

    /* The handler goes in BEFORE Subscribe(): a notification that arrives with no
     * handler installed is dropped. */
    proxy.SetReceiveHandler([](ids::ecu::EventId notifier, ids::ecu::ByteView payload) {
        std::uint32_t counter = 0U;
        if (payload.size() >= kCounterBytes)
        {
            counter = ((static_cast<std::uint32_t>(payload[0]) << 24U) | (static_cast<std::uint32_t>(payload[1]) << 16U) |
                       (static_cast<std::uint32_t>(payload[2]) << 8U) | static_cast<std::uint32_t>(payload[3]));
        }
        else
        {
            /* Payload shorter than the counter: leave it at zero. */
        }
        const std::uint32_t n = g_received.fetch_add(1U) + 1U;
        std::printf("[hmi_ecu] notification %u of event 0x%04X, %zu bytes, counter=%u\n", n, static_cast<unsigned>(notifier),
                    payload.size(), counter);
        std::fflush(stdout);
    });

    const ara::core::Result<void> started = proxy.Start();
    if (!started.HasValue())
    {
        std::printf("[hmi_ecu] ERROR starting the stack: %s\n", started.Error().Message());
        return 1;
    }
    else
    {
        /* Stack coming up, and the service has been requested. */
    }

    const ara::core::Result<void> registered = proxy.WaitUntilRegistered(kRegistrationWait);
    if (!registered.HasValue())
    {
        std::printf("[hmi_ecu] ERROR: %s\n", registered.Error().Message());
        return 1;
    }
    else
    {
        std::printf("[hmi_ecu] registered with the routing manager\n");
    }
    std::fflush(stdout);

    const ara::core::Result<void> available = proxy.WaitUntilAvailable(kAvailabilityWait);
    if (!available.HasValue())
    {
        std::printf("[hmi_ecu] ERROR: %s\n", available.Error().Message());
        std::printf("[hmi_ecu] registered but the service was never announced: suspect the multicast route\n");
        return 1;
    }
    else
    {
        std::printf("[hmi_ecu] service found through Service Discovery\n");
    }
    std::fflush(stdout);

    const ids::ecu::EventgroupId eventgroups[] = {mtx::GnssService::Eventgroups::Group4455()};

    const ara::core::Result<void> subscribed =
        proxy.Subscribe(mtx::GnssService::Events::Position(), ids::ecu::EventgroupIds(eventgroups));
    if (!subscribed.HasValue())
    {
        std::printf("[hmi_ecu] ERROR subscribing: %s\n", subscribed.Error().Message());
        return 1;
    }
    else
    {
        std::printf("[hmi_ecu] subscribed to eventgroup 0x%04X\n", static_cast<unsigned>(eventgroups[0]));
    }
    std::fflush(stdout);

    while (!g_stop.load())
    {
        std::this_thread::sleep_for(kIdlePoll);
    }

    std::printf("[hmi_ecu] shutting down after %u notifications\n", g_received.load());
    return 0;
}
