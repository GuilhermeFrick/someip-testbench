/**
 * @file      Proxy.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Consumer side of the libecu facade: finds a service and subscribes to its events.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   Mirror of ::ids::ecu::Skeleton, and it hides the same kind of ordering. A consumer
 *            cannot simply subscribe: it has to request the service, wait for Service Discovery to
 *            report it available, and only then subscribe to an eventgroup. Getting that order
 *            wrong does not fail loudly, it just never receives anything.
 *
 * @details   Two waits, and they are different things. WaitUntilRegistered() is about OUR stack
 *            being up; WaitUntilAvailable() is about the REMOTE service having been found over
 *            Service Discovery. A consumer that is registered but whose service never becomes
 *            available is the exact symptom of the missing multicast route, so the second wait
 *            reports ::ids::ecu::EcuErrc::ServiceUnavailableTimeout rather than hanging.
 */
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "src/ara/core/Result.hpp"
#include "src/libecu/EcuErrorDomain.hpp"
#include "src/libecu/Types.hpp"

/** @addtogroup LibEcuProxy Proxy
 *  @ingroup LibEcu
 *  @{
 */

namespace ids
{
namespace ecu
{

/**
 * @brief Called when a subscribed event arrives.
 * @param[in] notifier Event that carried the value.
 * @param[in] payload  Bytes received.
 * @warning Runs on the stack event-loop thread, NOT on the thread that created the proxy, and
 *          @p payload is a view that is only valid for the duration of the call. Copy what is
 *          needed and return quickly: blocking here blocks the whole stack.
 */
using EventHandler = std::function<void(EventId notifier, ByteView payload)>;

/**
 * @brief What a proxy needs in order to find a service.
 * @details The values come from the generated build/cfg/CommMatrix.hpp, never written by hand.
 */
struct ProxyConfig
{
    std::string appName;       ///< Application name; must match an entry of the generated vsomeip configuration.
    ServiceId   service  = 0U; ///< Service to look for.
    InstanceId  instance = 0U; ///< Instance of the service to look for.
};

/**
 * @brief Finds one service instance and receives its events.
 */
class Proxy final
{
  public:
    /**
     * @brief Build a proxy and bind it to the vsomeip application.
     * @param[in] cfg Service to look for and the application name to bind to.
     * @return The proxy, or an error describing why it could not be built.
     * @retval ::ids::ecu::EcuErrc::InvalidConfiguration    The application name is empty, or an
     *                                                     identifier is zero.
     * @retval ::ids::ecu::EcuErrc::ApplicationCreateFailed The vsomeip runtime refused to create
     *                                                     the application.
     * @retval ::ids::ecu::EcuErrc::ApplicationInitFailed   @c init() refused; almost always a
     *                                                     configuration that does not name this
     *                                                     application.
     */
    static ara::core::Result<std::unique_ptr<Proxy>> Create(const ProxyConfig &cfg);

    /**
     * @brief Stop the stack and release the application.
     * @return None.
     */
    ~Proxy();

    /**
     * @brief Install the handler that receives subscribed events.
     * @param[in] handler Callback invoked for every event that arrives; pass an empty function to
     *                    remove it.
     * @return None.
     * @warning Set this BEFORE Subscribe(), otherwise the first notifications arrive with nowhere
     *          to go and are dropped.
     */
    void SetReceiveHandler(EventHandler handler);

    /**
     * @brief Start the stack on its own thread and request the service.
     * @return Nothing on success, or an error.
     * @retval ::ids::ecu::EcuErrc::AlreadyStarted Start() was already called on this object.
     */
    ara::core::Result<void> Start();

    /**
     * @brief Block until the routing manager has registered this application.
     * @param[in] timeout How long to wait.
     * @return Nothing once registered, or an error on timeout.
     * @retval ::ids::ecu::EcuErrc::NotStarted          Start() was not called.
     * @retval ::ids::ecu::EcuErrc::RegistrationTimeout Registration did not arrive within @p timeout.
     */
    ara::core::Result<void> WaitUntilRegistered(std::chrono::milliseconds timeout);

    /**
     * @brief Block until Service Discovery reports the service as available.
     * @param[in] timeout How long to wait.
     * @return Nothing once available, or an error on timeout.
     * @retval ::ids::ecu::EcuErrc::NotStarted                Start() was not called.
     * @retval ::ids::ecu::EcuErrc::ServiceUnavailableTimeout The service was not announced within
     *                                                       @p timeout.
     * @details A timeout here, with registration already done, is the signature of the provider
     *          not being announced: either it is not running, or the multicast route is missing on
     *          one of the two nodes. See platform/sd-smoke/README.md.
     */
    ara::core::Result<void> WaitUntilAvailable(std::chrono::milliseconds timeout);

    /**
     * @brief Subscribe to an event.
     * @param[in] notifier    Event or field identifier to receive.
     * @param[in] eventgroups Eventgroups to subscribe to; must not be empty.
     * @return Nothing on success, or an error.
     * @retval ::ids::ecu::EcuErrc::NotStarted           Start() was not called.
     * @retval ::ids::ecu::EcuErrc::InvalidConfiguration @p notifier is zero or @p eventgroups is empty.
     * @details Requests the event and subscribes to its eventgroups. Call it after
     *          WaitUntilAvailable(), and after SetReceiveHandler().
     */
    ara::core::Result<void> Subscribe(EventId notifier, EventgroupIds eventgroups);

    /**
     * @brief Whether the routing manager has registered this application.
     * @return Whether our own stack is up.
     * @retval true  Registered.
     * @retval false Not registered.
     */
    bool IsRegistered() const noexcept;

    /**
     * @brief Whether Service Discovery currently reports the service as available.
     * @return Whether the remote service was found.
     * @retval true  Available.
     * @retval false Not available.
     */
    bool IsAvailable() const noexcept;

    /**
     * @brief Copy construction is deleted: a proxy owns a stack binding and a thread.
     * @return None.
     */
    Proxy(const Proxy &) = delete;

    /**
     * @brief Copy assignment is deleted: a proxy owns a stack binding and a thread.
     * @return Nothing; the operator cannot be called.
     */
    Proxy &operator=(const Proxy &) = delete;

  private:
    struct Impl; ///< vsomeip application, thread and subscription state, kept out of this header.

    /**
     * @brief Construct from an already initialised application.
     * @param[in,out] impl Initialised state, taken over by this proxy.
     * @return None.
     * @details Private so that Create() is the only way in.
     */
    explicit Proxy(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl; ///< Owned stack binding.
};

} // namespace ecu
} // namespace ids

/** @}*/ // End of LibEcuProxy
