/**
 * @file      Proxy.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Consumer side of the libecu facade, over vsomeip 3.6.4.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 */
#include "src/libecu/Proxy.hpp"

#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

#include <vsomeip/vsomeip.hpp>

namespace ids
{
namespace ecu
{

/**
 * @brief vsomeip application, event-loop thread and the subscription state.
 */
struct Proxy::Impl
{
    ProxyConfig                           cfg;                ///< Service and application being consumed.
    std::shared_ptr<vsomeip::application> app;                ///< The vsomeip application.
    std::thread                           loop;               ///< Thread running application::start().
    mutable std::mutex                    mutex;              ///< Guards the flags and the handler below.
    std::condition_variable               cv;                 ///< Signalled on registration and on availability.
    bool                                  registered = false; ///< Whether the routing manager registered us.
    bool                                  available  = false; ///< Whether Service Discovery found the service.
    bool                                  started    = false; ///< Whether Start() ran.
    EventHandler                          handler;            ///< Callback for arriving events.
};

namespace
{

/**
 * @brief Whether every identifier of a configuration is usable.
 * @param[in] cfg Configuration to check.
 * @return Whether the configuration can be used to look for a service.
 * @retval true  Name is non-empty and both identifiers are non-zero.
 * @retval false Something is missing.
 */
bool ConfigIsUsable(const ProxyConfig &cfg)
{
    return ((!cfg.appName.empty()) && (cfg.service != 0U) && (cfg.instance != 0U));
}

} // namespace

Proxy::Proxy(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl))
{
}

ara::core::Result<std::unique_ptr<Proxy>> Proxy::Create(const ProxyConfig &cfg)
{
    using Res = ara::core::Result<std::unique_ptr<Proxy>>;

    if (!ConfigIsUsable(cfg))
    {
        return Res::FromError(ara::core::ErrorCode(EcuErrc::InvalidConfiguration));
    }

    std::unique_ptr<Impl> impl(new Impl());
    impl->cfg = cfg;
    impl->app = vsomeip::runtime::get()->create_application(cfg.appName);
    if (!impl->app)
    {
        return Res::FromError(ara::core::ErrorCode(EcuErrc::ApplicationCreateFailed));
    }

    if (!impl->app->init())
    {
        return Res::FromError(ara::core::ErrorCode(EcuErrc::ApplicationInitFailed));
    }

    Impl *raw = impl.get();

    raw->app->register_state_handler([raw](vsomeip::state_type_e state) {
        {
            std::lock_guard<std::mutex> lock(raw->mutex);
            raw->registered = (state == vsomeip::state_type_e::ST_REGISTERED);
        }
        raw->cv.notify_all();
    });

    /* Availability is a different thing from registration: it says Service
     * Discovery FOUND the provider. It is the signal that never comes when the
     * multicast route is missing on one of the two nodes. */
    raw->app->register_availability_handler(cfg.service, cfg.instance,
                                            [raw](vsomeip::service_t, vsomeip::instance_t, bool isAvailable) {
                                                {
                                                    std::lock_guard<std::mutex> lock(raw->mutex);
                                                    raw->available = isAvailable;
                                                }
                                                raw->cv.notify_all();
                                            });

    return Res::FromValue(std::unique_ptr<Proxy>(new Proxy(std::move(impl))));
}

Proxy::~Proxy()
{
    if (m_impl)
    {
        if (m_impl->app)
        {
            m_impl->app->clear_all_handler();
            m_impl->app->stop();
        }
        else
        {
            /* No application: nothing to stop. */
        }
        if (m_impl->loop.joinable())
        {
            m_impl->loop.join();
        }
        else
        {
            /* The loop never came up. */
        }
    }
    else
    {
        /* Moved-from object: nothing to release. */
    }
}

void Proxy::SetReceiveHandler(EventHandler handler)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->handler = std::move(handler);
}

ara::core::Result<void> Proxy::Start()
{
    using Res = ara::core::Result<void>;

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->started)
        {
            return Res::FromError(ara::core::ErrorCode(EcuErrc::AlreadyStarted));
        }
        else
        {
            m_impl->started = true;
        }
    }

    /* Request the service before starting the loop: Service Discovery only
     * looks for what has been requested. */
    m_impl->app->request_service(m_impl->cfg.service, m_impl->cfg.instance);

    Impl *raw    = m_impl.get();
    m_impl->loop = std::thread([raw] { raw->app->start(); });
    return Res::FromValue();
}

ara::core::Result<void> Proxy::WaitUntilRegistered(std::chrono::milliseconds timeout)
{
    using Res = ara::core::Result<void>;

    std::unique_lock<std::mutex> lock(m_impl->mutex);
    if (!m_impl->started)
    {
        return Res::FromError(ara::core::ErrorCode(EcuErrc::NotStarted));
    }
    else
    {
        /* Started: waiting is allowed. */
    }

    if (!m_impl->cv.wait_for(lock, timeout, [this] { return m_impl->registered; }))
    {
        return Res::FromError(ara::core::ErrorCode(EcuErrc::RegistrationTimeout));
    }
    else
    {
        /* Registered within the timeout. */
    }
    return Res::FromValue();
}

ara::core::Result<void> Proxy::WaitUntilAvailable(std::chrono::milliseconds timeout)
{
    using Res = ara::core::Result<void>;

    std::unique_lock<std::mutex> lock(m_impl->mutex);
    if (!m_impl->started)
    {
        return Res::FromError(ara::core::ErrorCode(EcuErrc::NotStarted));
    }
    else
    {
        /* Started: waiting is allowed. */
    }

    if (!m_impl->cv.wait_for(lock, timeout, [this] { return m_impl->available; }))
    {
        return Res::FromError(ara::core::ErrorCode(EcuErrc::ServiceUnavailableTimeout));
    }
    else
    {
        /* Found within the timeout. */
    }
    return Res::FromValue();
}

ara::core::Result<void> Proxy::Subscribe(EventId notifier, EventgroupIds eventgroups)
{
    using Res = ara::core::Result<void>;

    if ((notifier == 0U) || eventgroups.empty())
    {
        return Res::FromError(ara::core::ErrorCode(EcuErrc::InvalidConfiguration));
    }
    else
    {
        /* Arguments are usable. */
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->started)
        {
            return Res::FromError(ara::core::ErrorCode(EcuErrc::NotStarted));
        }
        else
        {
            /* Stack is up. */
        }
    }

    std::set<vsomeip::eventgroup_t> groups;
    for (const EventgroupId g : eventgroups)
    {
        groups.insert(static_cast<vsomeip::eventgroup_t>(g));
    }

    /* The message handler of an event is registered with the EVENT id in place
     * of the method: in SOME/IP a notification is just another message id. */
    Impl *raw = m_impl.get();
    m_impl->app->register_message_handler(m_impl->cfg.service, m_impl->cfg.instance, static_cast<vsomeip::method_t>(notifier),
                                          [raw](const std::shared_ptr<vsomeip::message> &msg) {
                                              EventHandler h;
                                              {
                                                  std::lock_guard<std::mutex> lock(raw->mutex);
                                                  h = raw->handler;
                                              }
                                              if (!h)
                                              {
                                                  /* Nobody installed a handler: drop the notification. */
                                                  return;
                                              }
                                              else
                                              {
                                                  /* A handler is installed. */
                                              }
                                              const std::shared_ptr<vsomeip::payload> p = msg->get_payload();
                                              const ByteView                          view(p->get_data(), p->get_length());
                                              h(static_cast<EventId>(msg->get_method()), view);
                                          });

    m_impl->app->request_event(m_impl->cfg.service, m_impl->cfg.instance, static_cast<vsomeip::event_t>(notifier), groups);
    for (const vsomeip::eventgroup_t g : groups)
    {
        m_impl->app->subscribe(m_impl->cfg.service, m_impl->cfg.instance, g);
    }
    return Res::FromValue();
}

bool Proxy::IsRegistered() const noexcept
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->registered;
}

bool Proxy::IsAvailable() const noexcept
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->available;
}

} // namespace ecu
} // namespace ids
