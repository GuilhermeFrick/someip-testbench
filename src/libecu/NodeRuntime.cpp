/**
 * @file      NodeRuntime.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     One vsomeip application hosting several service instances, over vsomeip 3.6.4.
 * @version   1.0
 * @date      2026-11-01
 * @copyright Copyright (c) 2026
 */
#include "src/libecu/NodeRuntime.hpp"

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

/* These live in a NAMED namespace, not an anonymous one, because they become fields of
 * NodeRuntime::Impl, and a type with external linkage holding a field from the anonymous namespace
 * is what -Wsubobject-linkage exists to complain about. The helper FUNCTIONS below are anonymous:
 * for a function there is no such objection. */
namespace detail
{

/**
 * @brief One service instance this node offers, and the events declared on it.
 */
struct Provided
{
    ServiceId         service  = 0U;    ///< Service identifier.
    InstanceId        instance = 0U;    ///< Instance identifier.
    std::set<EventId> events;           ///< Events declared by ProvideEvent(), so Send() can refuse an undeclared one.
    bool              offered  = false; ///< Whether the offer is currently up on the stack.
};

/**
 * @brief One service instance this node consumes, and whether it has been found.
 */
struct Consumed
{
    ServiceId  service   = 0U;    ///< Service identifier.
    InstanceId instance  = 0U;    ///< Instance identifier.
    bool       available = false; ///< Whether Service Discovery currently reports it.
};

} // namespace detail

using detail::Consumed;
using detail::Provided;

/**
 * @brief vsomeip application, event-loop thread and the per-instance state.
 * @details The two vectors are only appended to BEFORE Start(), so a pointer into either of them
 *          is stable for as long as the lock that produced it is held. Nothing stores one.
 */
struct NodeRuntime::Impl
{
    std::string                           appName;              ///< Application name, as the configuration spells it.
    std::shared_ptr<vsomeip::application> app;                  ///< The one vsomeip application of this node.
    std::thread                           loop;                 ///< Thread running application::start().
    mutable std::mutex                    mutex;                ///< Guards everything below.
    std::condition_variable               cv;                   ///< Signalled on registration and on availability.
    bool                                  registered  = false;  ///< Whether the routing manager registered us.
    bool                                  started     = false;  ///< Whether Start() ran.
    bool                                  offerWanted = false;  ///< Whether Offer() was called.
    std::vector<Provided>                 provided;             ///< Instances offered.
    std::vector<Consumed>                 consumed;             ///< Instances consumed.
    NodeEventHandler                      handler;              ///< Callback for arriving events.
};

namespace
{

/**
 * @brief Find a provided instance.
 * @param[in] all      Instances to search.
 * @param[in] service  Service identifier wanted.
 * @param[in] instance Instance identifier wanted.
 * @return Pointer to the entry, or null when it is not declared.
 * @details Takes a pointer in rather than an index out, because an index would need a sentinel and
 *          the caller already holds the lock that makes the pointer meaningful.
 */
Provided *AchaProvido(std::vector<Provided> &all, ServiceId service, InstanceId instance)
{
    Provided *found = nullptr;
    for (std::size_t i = 0U; (i < all.size()) && (found == nullptr); ++i)
    {
        if ((all[i].service == service) && (all[i].instance == instance))
        {
            found = &all[i];
        }
        else
        {
            /* Another instance: keep looking. */
        }
    }
    return found;
}

/**
 * @brief Find a consumed instance.
 * @param[in] all      Instances to search.
 * @param[in] service  Service identifier wanted.
 * @param[in] instance Instance identifier wanted.
 * @return Pointer to the entry, or null when it is not declared.
 */
Consumed *AchaConsumido(std::vector<Consumed> &all, ServiceId service, InstanceId instance)
{
    Consumed *found = nullptr;
    for (std::size_t i = 0U; (i < all.size()) && (found == nullptr); ++i)
    {
        if ((all[i].service == service) && (all[i].instance == instance))
        {
            found = &all[i];
        }
        else
        {
            /* Another instance: keep looking. */
        }
    }
    return found;
}

/**
 * @brief Claim the provided instances that still need offering, marking them as offered.
 * @param[in,out] provided    Instances offered by this node.
 * @param[in]     registered  Whether the routing manager has registered the application.
 * @param[in]     offerWanted Whether Offer() has been called.
 * @return Copies of the instances this caller is now responsible for offering; empty when the stack
 *         is not ready or nobody asked for an offer.
 * @details Claiming and marking happen under ONE lock held by the caller, and the copies are what
 *          the caller passes to the stack afterwards. Offer() and the state handler can both find
 *          themselves registered with an offer pending, and marking only after the call would let
 *          both offer the same instance. Returning copies also means nothing holds a pointer into
 *          the vector.
 * @details Takes the fields rather than the runtime state, because NodeRuntime::Impl is private and
 *          a free function may not name it.
 */
std::vector<Provided> ReclamaNaoOferecidos(std::vector<Provided> &provided, bool registered, bool offerWanted)
{
    std::vector<Provided> claimed;
    if (registered && offerWanted)
    {
        for (std::size_t i = 0U; i < provided.size(); ++i)
        {
            if (!provided[i].offered)
            {
                provided[i].offered = true;
                claimed.push_back(provided[i]);
            }
            else
            {
                /* Already offered, by this call or by the other path. */
            }
        }
    }
    else
    {
        /* Either not registered, or nobody asked for the offer yet. */
    }
    return claimed;
}

/**
 * @brief Translate the transport of the matrix into what vsomeip calls reliability.
 * @param[in] transport Endpoint the events travel on.
 * @return The matching vsomeip value.
 */
vsomeip::reliability_type_e ComoReliability(Transport transport)
{
    vsomeip::reliability_type_e r = vsomeip::reliability_type_e::RT_UNKNOWN;
    switch (transport)
    {
        case Transport::Reliable:
            r = vsomeip::reliability_type_e::RT_RELIABLE;
            break;
        case Transport::Unreliable:
            r = vsomeip::reliability_type_e::RT_UNRELIABLE;
            break;
        case Transport::Both:
            r = vsomeip::reliability_type_e::RT_BOTH;
            break;
        case Transport::Unspecified:
        default:
            /* RT_UNKNOWN: the stack decides, which is what produced the 65/35 split. */
            break;
    }
    return r;
}

/**
 * @brief Turn a list of eventgroup identifiers into the set vsomeip asks for.
 * @param[in] eventgroups Identifiers to convert.
 * @return The same identifiers as a vsomeip set.
 */
std::set<vsomeip::eventgroup_t> ComoConjunto(EventgroupIds eventgroups)
{
    std::set<vsomeip::eventgroup_t> groups;
    for (const EventgroupId g : eventgroups)
    {
        groups.insert(static_cast<vsomeip::eventgroup_t>(g));
    }
    return groups;
}

} // namespace

NodeRuntime::NodeRuntime(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl))
{
}

ara::core::Result<std::unique_ptr<NodeRuntime>> NodeRuntime::Create(const std::string &appName)
{
    using Res = ara::core::Result<std::unique_ptr<NodeRuntime>>;

    if (appName.empty())
    {
        return Res::FromError(MakeErrorCode(EcuErrc::InvalidConfiguration));
    }
    else
    {
        /* Name is usable. */
    }

    std::unique_ptr<Impl> impl(new Impl());
    impl->appName = appName;
    impl->app     = vsomeip::runtime::get()->create_application(appName);
    if (!impl->app)
    {
        return Res::FromError(MakeErrorCode(EcuErrc::ApplicationCreateFailed));
    }
    else
    {
        /* The runtime handed out an application. */
    }

    if (!impl->app->init())
    {
        return Res::FromError(MakeErrorCode(EcuErrc::ApplicationInitFailed));
    }
    else
    {
        /* Configuration names this application. */
    }

    /* vsomeip only accepts offer_service once the routing manager has registered the application,
     * and ignores an earlier one without a word. Offer() records the intent and this handler
     * applies it, for every instance at once. */
    Impl *raw = impl.get();
    raw->app->register_state_handler([raw](vsomeip::state_type_e state) {
        std::vector<Provided> aOferecer;
        {
            std::lock_guard<std::mutex> lock(raw->mutex);
            raw->registered = (state == vsomeip::state_type_e::ST_REGISTERED);
            aOferecer       = ReclamaNaoOferecidos(raw->provided, raw->registered, raw->offerWanted);
        }
        for (std::size_t i = 0U; i < aOferecer.size(); ++i)
        {
            raw->app->offer_service(aOferecer[i].service, aOferecer[i].instance);
        }
        raw->cv.notify_all();
    });

    return Res::FromValue(std::unique_ptr<NodeRuntime>(new NodeRuntime(std::move(impl))));
}

NodeRuntime::~NodeRuntime()
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

ara::core::Result<void> NodeRuntime::ProvideService(ServiceId service, InstanceId instance)
{
    using Res = ara::core::Result<void>;

    if ((service == 0U) || (instance == 0U))
    {
        return Res::FromError(MakeErrorCode(EcuErrc::InvalidConfiguration));
    }
    else
    {
        /* Identifiers are usable. */
    }

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->started)
    {
        return Res::FromError(MakeErrorCode(EcuErrc::AlreadyStarted));
    }
    else if (AchaProvido(m_impl->provided, service, instance) != nullptr)
    {
        return Res::FromError(MakeErrorCode(EcuErrc::ServiceAlreadyDeclared));
    }
    else
    {
        /* New instance, and the stack is not up yet. */
    }

    Provided entry;
    entry.service  = service;
    entry.instance = instance;
    m_impl->provided.push_back(entry);
    return Res::FromValue();
}

ara::core::Result<void> NodeRuntime::ProvideEvent(ServiceId service, InstanceId instance, EventId notifier, EventgroupIds eventgroups,
                                                  bool isField, std::chrono::milliseconds cycle, Transport transport)
{
    using Res = ara::core::Result<void>;

    if ((notifier == 0U) || eventgroups.empty())
    {
        return Res::FromError(MakeErrorCode(EcuErrc::InvalidConfiguration));
    }
    else
    {
        /* Arguments are usable. */
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->started)
        {
            return Res::FromError(MakeErrorCode(EcuErrc::AlreadyStarted));
        }
        else
        {
            /* Still in the declaration phase. */
        }

        Provided *const owner = AchaProvido(m_impl->provided, service, instance);
        if (owner == nullptr)
        {
            return Res::FromError(MakeErrorCode(EcuErrc::ServiceNotDeclared));
        }
        else
        {
            owner->events.insert(notifier);
        }
    }

    /* The three arguments between the cycle and the reliability are the vsomeip defaults, spelled
     * out because the reliability comes after them and C++ has no named arguments. */
    const vsomeip::event_type_e kind = isField ? vsomeip::event_type_e::ET_FIELD : vsomeip::event_type_e::ET_EVENT;
    m_impl->app->offer_event(service, instance, static_cast<vsomeip::event_t>(notifier), ComoConjunto(eventgroups), kind, cycle, false, true,
                             nullptr, ComoReliability(transport));
    return Res::FromValue();
}

ara::core::Result<void> NodeRuntime::ConsumeService(ServiceId service, InstanceId instance)
{
    using Res = ara::core::Result<void>;

    if ((service == 0U) || (instance == 0U))
    {
        return Res::FromError(MakeErrorCode(EcuErrc::InvalidConfiguration));
    }
    else
    {
        /* Identifiers are usable. */
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->started)
        {
            return Res::FromError(MakeErrorCode(EcuErrc::AlreadyStarted));
        }
        else if (AchaConsumido(m_impl->consumed, service, instance) != nullptr)
        {
            return Res::FromError(MakeErrorCode(EcuErrc::ServiceAlreadyDeclared));
        }
        else
        {
            /* New instance, and the stack is not up yet. */
        }

        Consumed entry;
        entry.service  = service;
        entry.instance = instance;
        m_impl->consumed.push_back(entry);
    }

    /* Availability is a different thing from registration: it says Service Discovery FOUND the
     * provider. It is the signal that never comes when the multicast route is missing. The handler
     * looks the entry up by identifier instead of capturing a pointer, because the vector may
     * still grow while other instances are declared. */
    Impl *raw = m_impl.get();
    m_impl->app->register_availability_handler(service, instance,
                                               [raw](vsomeip::service_t s, vsomeip::instance_t i, bool isAvailable) {
                                                   {
                                                       std::lock_guard<std::mutex> lock(raw->mutex);
                                                       Consumed *const entry = AchaConsumido(raw->consumed, static_cast<ServiceId>(s),
                                                                                             static_cast<InstanceId>(i));
                                                       if (entry != nullptr)
                                                       {
                                                           entry->available = isAvailable;
                                                       }
                                                       else
                                                       {
                                                           /* Not an instance we asked for. */
                                                       }
                                                   }
                                                   raw->cv.notify_all();
                                               });
    return Res::FromValue();
}

void NodeRuntime::SetReceiveHandler(NodeEventHandler handler)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->handler = std::move(handler);
}

ara::core::Result<void> NodeRuntime::Start()
{
    using Res = ara::core::Result<void>;

    std::vector<Consumed> aPedir;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->started)
        {
            return Res::FromError(MakeErrorCode(EcuErrc::AlreadyStarted));
        }
        else
        {
            m_impl->started = true;
            aPedir          = m_impl->consumed;
        }
    }

    /* Request every consumed service before starting the loop: Service Discovery only looks for
     * what has been requested. */
    for (std::size_t i = 0U; i < aPedir.size(); ++i)
    {
        m_impl->app->request_service(aPedir[i].service, aPedir[i].instance);
    }

    /* start() runs the event loop and never returns: it goes on its own thread. */
    Impl *raw    = m_impl.get();
    m_impl->loop = std::thread([raw] { raw->app->start(); });
    return Res::FromValue();
}

ara::core::Result<void> NodeRuntime::WaitUntilRegistered(std::chrono::milliseconds timeout)
{
    using Res = ara::core::Result<void>;

    std::unique_lock<std::mutex> lock(m_impl->mutex);
    if (!m_impl->started)
    {
        return Res::FromError(MakeErrorCode(EcuErrc::NotStarted));
    }
    else
    {
        /* Started: waiting is allowed. */
    }

    if (!m_impl->cv.wait_for(lock, timeout, [this] { return m_impl->registered; }))
    {
        return Res::FromError(MakeErrorCode(EcuErrc::RegistrationTimeout));
    }
    else
    {
        /* Registered within the timeout. */
    }
    return Res::FromValue();
}

ara::core::Result<void> NodeRuntime::Offer()
{
    using Res = ara::core::Result<void>;

    std::vector<Provided> aOferecer;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->started)
        {
            return Res::FromError(MakeErrorCode(EcuErrc::NotStarted));
        }
        else
        {
            m_impl->offerWanted = true;
        }

        /* Empty when we are not registered yet, and then the state handler applies the offer. */
        aOferecer = ReclamaNaoOferecidos(m_impl->provided, m_impl->registered, m_impl->offerWanted);
    }

    for (std::size_t i = 0U; i < aOferecer.size(); ++i)
    {
        m_impl->app->offer_service(aOferecer[i].service, aOferecer[i].instance);
    }
    return Res::FromValue();
}

void NodeRuntime::StopOffer() noexcept
{
    std::vector<Provided> aRetirar;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->offerWanted = false;
        for (std::size_t i = 0U; i < m_impl->provided.size(); ++i)
        {
            if (m_impl->provided[i].offered)
            {
                aRetirar.push_back(m_impl->provided[i]);
                m_impl->provided[i].offered = false;
            }
            else
            {
                /* There was no offer up for this instance. */
            }
        }
    }

    for (std::size_t i = 0U; i < aRetirar.size(); ++i)
    {
        m_impl->app->stop_offer_service(aRetirar[i].service, aRetirar[i].instance);
    }
}

ara::core::Result<void> NodeRuntime::WaitUntilAvailable(ServiceId service, InstanceId instance, std::chrono::milliseconds timeout)
{
    using Res = ara::core::Result<void>;

    std::unique_lock<std::mutex> lock(m_impl->mutex);
    if (!m_impl->started)
    {
        return Res::FromError(MakeErrorCode(EcuErrc::NotStarted));
    }
    else if (AchaConsumido(m_impl->consumed, service, instance) == nullptr)
    {
        return Res::FromError(MakeErrorCode(EcuErrc::ServiceNotDeclared));
    }
    else
    {
        /* Declared, and the stack is up. */
    }

    /* The predicate looks the entry up again on every wake-up, which is what keeps it correct even
     * though the pointer above was taken under this same lock. */
    const bool achou = m_impl->cv.wait_for(lock, timeout, [this, service, instance] {
        const Consumed *const entry = AchaConsumido(m_impl->consumed, service, instance);
        return (entry != nullptr) && entry->available;
    });
    if (!achou)
    {
        return Res::FromError(MakeErrorCode(EcuErrc::ServiceUnavailableTimeout));
    }
    else
    {
        /* Found within the timeout. */
    }
    return Res::FromValue();
}

ara::core::Result<void> NodeRuntime::Subscribe(ServiceId service, InstanceId instance, EventId notifier, EventgroupIds eventgroups,
                                               Transport transport)
{
    using Res = ara::core::Result<void>;

    if ((notifier == 0U) || eventgroups.empty())
    {
        return Res::FromError(MakeErrorCode(EcuErrc::InvalidConfiguration));
    }
    else
    {
        /* Arguments are usable. */
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->started)
        {
            return Res::FromError(MakeErrorCode(EcuErrc::NotStarted));
        }
        else if (AchaConsumido(m_impl->consumed, service, instance) == nullptr)
        {
            return Res::FromError(MakeErrorCode(EcuErrc::ServiceNotDeclared));
        }
        else
        {
            /* Declared, and the stack is up. */
        }
    }

    const std::set<vsomeip::eventgroup_t> groups = ComoConjunto(eventgroups);

    /* The message handler of an event is registered with the EVENT id in place of the method: in
     * SOME/IP a notification is just another message id. The service and the instance come from
     * the message itself, because this one handler serves several services at once and the event
     * identifiers repeat between them. */
    Impl *raw = m_impl.get();
    m_impl->app->register_message_handler(service, instance, static_cast<vsomeip::method_t>(notifier),
                                          [raw](const std::shared_ptr<vsomeip::message> &msg) {
                                              NodeEventHandler h;
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
                                              h(static_cast<ServiceId>(msg->get_service()), static_cast<InstanceId>(msg->get_instance()),
                                                static_cast<EventId>(msg->get_method()), view);
                                          });

    /* The consumer states the transport too: request_event with RT_UNKNOWN makes the stack accept
     * the event on either endpoint, and then which one carries it is whatever the provider chose.
     *
     * The type stays ET_EVENT even for a field, which is what ::ids::ecu::Proxy also does. It is a
     * KNOWN and unmeasured difference: for a field the stack would request the initial value on
     * subscribing, so a faithful reproduction may owe a few messages per subscription. It is not
     * what the transport finding was about, and claiming it is right without having measured it
     * would be the same mistake again. */
    m_impl->app->request_event(service, instance, static_cast<vsomeip::event_t>(notifier), groups,
                               vsomeip::event_type_e::ET_EVENT, ComoReliability(transport));
    for (std::set<vsomeip::eventgroup_t>::const_iterator it = groups.begin(); it != groups.end(); ++it)
    {
        m_impl->app->subscribe(service, instance, *it);
    }
    return Res::FromValue();
}

ara::core::Result<void> NodeRuntime::Send(ServiceId service, InstanceId instance, EventId notifier, ByteView payload)
{
    using Res = ara::core::Result<void>;

    if (payload.size() > kMaxPayloadBytes)
    {
        return Res::FromError(MakeErrorCode(EcuErrc::PayloadTooLarge));
    }
    else
    {
        /* Fits in one message. */
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->started)
        {
            return Res::FromError(MakeErrorCode(EcuErrc::NotStarted));
        }
        else
        {
            /* Stack is up. */
        }

        const Provided *const owner = AchaProvido(m_impl->provided, service, instance);
        if (owner == nullptr)
        {
            return Res::FromError(MakeErrorCode(EcuErrc::ServiceNotDeclared));
        }
        else if (owner->events.find(notifier) == owner->events.end())
        {
            return Res::FromError(MakeErrorCode(EcuErrc::EventNotOffered));
        }
        else
        {
            /* Event declared on this instance. */
        }
    }

    const std::shared_ptr<vsomeip::payload> p = vsomeip::runtime::get()->create_payload();
    p->set_data(payload.data(), static_cast<vsomeip::length_t>(payload.size()));
    /* _force = true, e NAO e detalhe. Com o padrao (false) o vsomeip SUPRIME a
     * notificacao de um field cujo payload nao mudou, e o chamador nao fica
     * sabendo: notify() devolve void. Medido no barramento de referencia, 99,3%
     * a 99,6% das notificacoes consecutivas de Service1001 e Service1002 carregam
     * bytes IDENTICOS (72% em Service1003) -- e mesmo assim elas aparecem na
     * captura, na cadencia cheia. Ou seja: o barramento de referencia publica o
     * valor repetido, e suprimir seria reproduzir outra coisa.
     *
     * A primeira rodada com payload do dataset custou esta descoberta: os
     * provedores "enviaram" 27 862 e os consumidores receberam 1 403, sem um
     * unico erro em lugar nenhum. Sem o force, o contador de enviados conta
     * TENTATIVAS, nao mensagens. */
    m_impl->app->notify(service, instance, static_cast<vsomeip::event_t>(notifier), p, true);
    return Res::FromValue();
}

bool NodeRuntime::IsRegistered() const noexcept
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->registered;
}

bool NodeRuntime::IsAvailable(ServiceId service, InstanceId instance) const noexcept
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const Consumed *const       entry = AchaConsumido(m_impl->consumed, service, instance);
    return (entry != nullptr) && entry->available;
}

} // namespace ecu
} // namespace ids
