/**
 * @file      Skeleton.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Provider side of the libecu facade, over vsomeip 3.6.4.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   This is the only file of the facade that includes vsomeip. Everything above it sees
 *            ::ids::ecu::Skeleton and the identifier types, which is what keeps a node free of the
 *            stack.
 */
#include "src/libecu/Skeleton.hpp"

#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <vsomeip/vsomeip.hpp>

namespace ids
{
namespace ecu
{

/**
 * @brief vsomeip application, event-loop thread and the offer state.
 */
struct Skeleton::Impl
{
    SkeletonConfig                        cfg;                 ///< Service and application being offered.
    std::shared_ptr<vsomeip::application> app;                 ///< The vsomeip application.
    std::thread                           loop;                ///< Thread running application::start().
    mutable std::mutex                    mutex;               ///< Guards the flags below and the event set.
    std::condition_variable               registeredCv;        ///< Signalled when the registration state changes.
    bool                                  registered = false;  ///< Whether the routing manager registered us.
    bool                                  started    = false;  ///< Whether Start() ran.
    bool                                  offerWanted = false; ///< Offer() was called; apply it on registration.
    bool                                  offered    = false;  ///< The service is currently offered.
    std::set<EventId>                     events;              ///< Events declared through OfferEvent().
};

namespace
{

/**
 * @brief Whether every identifier of a configuration is usable.
 * @param[in] cfg Configuration to check.
 * @return Whether the configuration can be used to offer a service.
 * @retval true  Name is non-empty and both identifiers are non-zero.
 * @retval false Something is missing.
 */
bool ConfigIsUsable(const SkeletonConfig &cfg)
{
    return ((!cfg.appName.empty()) && (cfg.service != 0U) && (cfg.instance != 0U));
}

} // namespace

Skeleton::Skeleton(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl))
{
}

ara::core::Result<std::unique_ptr<Skeleton>> Skeleton::Create(const SkeletonConfig &cfg)
{
    using Res = ara::core::Result<std::unique_ptr<Skeleton>>;

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

    /* The state handler is what gets the ordering right: vsomeip only accepts
     * offer_service after ST_REGISTERED. Offer() records the intent in
     * offerWanted and this handler applies it, so the caller need not know. */
    Impl *raw = impl.get();
    raw->app->register_state_handler([raw](vsomeip::state_type_e state) {
        bool offerNow = false;
        {
            std::lock_guard<std::mutex> lock(raw->mutex);
            raw->registered = (state == vsomeip::state_type_e::ST_REGISTERED);
            if (raw->registered && raw->offerWanted && (!raw->offered))
            {
                offerNow = true;
            }
            else
            {
                /* Nothing to do: either we are not registered, or nobody asked
                 * for the offer, or it is already up. */
            }
        }
        if (offerNow)
        {
            raw->app->offer_service(raw->cfg.service, raw->cfg.instance);
            std::lock_guard<std::mutex> lock(raw->mutex);
            raw->offered = true;
        }
        else
        {
            /* No offer pending. */
        }
        raw->registeredCv.notify_all();
    });

    return Res::FromValue(std::unique_ptr<Skeleton>(new Skeleton(std::move(impl))));
}

Skeleton::~Skeleton()
{
    if (m_impl)
    {
        if (m_impl->app)
        {
            StopOffer();
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

ara::core::Result<void> Skeleton::OfferEvent(EventId notifier, EventgroupIds eventgroups, bool isField, std::chrono::milliseconds cycle)
{
    using Res = ara::core::Result<void>;

    if ((notifier == 0U) || eventgroups.empty())
    {
        return Res::FromError(ara::core::ErrorCode(EcuErrc::InvalidConfiguration));
    }

    std::set<vsomeip::eventgroup_t> groups;
    for (const EventgroupId g : eventgroups)
    {
        groups.insert(static_cast<vsomeip::eventgroup_t>(g));
    }

    const vsomeip::event_type_e kind = isField ? vsomeip::event_type_e::ET_FIELD : vsomeip::event_type_e::ET_EVENT;

    m_impl->app->offer_event(m_impl->cfg.service, m_impl->cfg.instance, static_cast<vsomeip::event_t>(notifier), groups, kind, cycle);

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->events.insert(notifier);
    }
    return Res::FromValue();
}

ara::core::Result<void> Skeleton::Start()
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

    /* start() runs the event loop and never returns: it goes on its own thread. */
    Impl *raw  = m_impl.get();
    m_impl->loop = std::thread([raw] { raw->app->start(); });
    return Res::FromValue();
}

ara::core::Result<void> Skeleton::WaitUntilRegistered(std::chrono::milliseconds timeout)
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

    const bool ok = m_impl->registeredCv.wait_for(lock, timeout, [this] { return m_impl->registered; });
    if (!ok)
    {
        return Res::FromError(ara::core::ErrorCode(EcuErrc::RegistrationTimeout));
    }
    else
    {
        /* Registered within the timeout. */
    }
    return Res::FromValue();
}

ara::core::Result<void> Skeleton::Offer()
{
    using Res = ara::core::Result<void>;

    bool offerNow = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->started)
        {
            return Res::FromError(ara::core::ErrorCode(EcuErrc::NotStarted));
        }
        else
        {
            m_impl->offerWanted = true;
            offerNow            = (m_impl->registered && (!m_impl->offered));
        }
    }

    if (offerNow)
    {
        m_impl->app->offer_service(m_impl->cfg.service, m_impl->cfg.instance);
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->offered = true;
    }
    else
    {
        /* Not registered yet: the state handler will apply the offer. */
    }
    return Res::FromValue();
}

void Skeleton::StopOffer() noexcept
{
    bool wasOffered = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        wasOffered          = m_impl->offered;
        m_impl->offerWanted = false;
        m_impl->offered     = false;
    }
    if (wasOffered)
    {
        m_impl->app->stop_offer_service(m_impl->cfg.service, m_impl->cfg.instance);
    }
    else
    {
        /* There was no offer up. */
    }
}

ara::core::Result<void> Skeleton::Send(EventId notifier, ByteView payload)
{
    using Res = ara::core::Result<void>;

    if (payload.size() > kMaxPayloadBytes)
    {
        return Res::FromError(ara::core::ErrorCode(EcuErrc::PayloadTooLarge));
    }
    else
    {
        /* Fits in one message. */
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->started)
        {
            return Res::FromError(ara::core::ErrorCode(EcuErrc::NotStarted));
        }
        else if (m_impl->events.find(notifier) == m_impl->events.end())
        {
            return Res::FromError(ara::core::ErrorCode(EcuErrc::EventNotOffered));
        }
        else
        {
            /* Event declared and stack up. */
        }
    }

    const std::shared_ptr<vsomeip::payload> p = vsomeip::runtime::get()->create_payload();
    p->set_data(payload.data(), static_cast<vsomeip::length_t>(payload.size()));
    m_impl->app->notify(m_impl->cfg.service, m_impl->cfg.instance, static_cast<vsomeip::event_t>(notifier), p);
    return Res::FromValue();
}

bool Skeleton::IsRegistered() const noexcept
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->registered;
}

} // namespace ecu
} // namespace ids
