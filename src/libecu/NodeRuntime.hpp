/**
 * @file      NodeRuntime.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     One vsomeip application hosting SEVERAL service instances, offered and consumed.
 * @version   1.0
 * @date      2026-11-01
 * @copyright Copyright (c) 2026
 *
 * @details   ::ids::ecu::Skeleton and ::ids::ecu::Proxy each bind ONE application to ONE service
 *            instance, which is the right shape for a node written by hand for one service. A node
 *            driven by the communication matrix is not that shape: in the recovered matrix one
 *            consumer subscribes to THREE different services, and a provider may come to offer more
 *            than one.
 *
 * @details   The obvious move would be three proxies with the same application name. It is wrong,
 *            and it is wrong SILENTLY, which is why this class exists. In vsomeip 3.6.4,
 *            @c runtime_impl::create_application appends a postfix when the name is already taken:
 *            @code
 *            auto found_application = applications_.find(_name);
 *            if (found_application != applications_.end()) {
 *                its_name += "_" + std::to_string(postfix_id++);
 *            }
 *            @endcode
 *            So the second proxy of a node called @c node-9 becomes @c node-9_0 -- a name the
 *            GENERATED vsomeip configuration does not mention. It does not fail: it gets an
 *            auto-assigned client identifier and none of the configured endpoints. The bus would
 *            then show several client identities behind one address, which is precisely the kind of
 *            difference that must not be introduced by accident into a reproduction.
 *
 * @details   One application per node is also what ara::com does: a process has one application and
 *            many skeletons and proxies inside it. What this class adds over the pair above is only
 *            that the service instance becomes an ARGUMENT instead of being fixed at construction.
 *
 * @details   The ordering traps are the same two the pair already documents, and they are handled
 *            the same way: an offer placed before registration does nothing, so Offer() records the
 *            intent and the state handler applies it; a service that was never REQUESTED is never
 *            looked for by Service Discovery, so ConsumeService() must run before Start().
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "src/ara/core/Result.hpp"
#include "src/libecu/EcuErrorDomain.hpp"
#include "src/libecu/Types.hpp"

/** @addtogroup LibEcuNodeRuntime NodeRuntime
 *  @ingroup LibEcu
 *  @{
 */

namespace ids
{
namespace ecu
{

/**
 * @brief Called when an event of any subscribed service arrives.
 * @param[in] service  Service that published it.
 * @param[in] instance Instance of that service.
 * @param[in] notifier Event that carried the value.
 * @param[in] payload  Bytes received.
 * @warning Runs on the stack event-loop thread, and @p payload is only valid for the duration of
 *          the call. Unlike ::ids::ecu::EventHandler this one also carries the service, because a
 *          single runtime receives from several of them and the event identifiers repeat: in the
 *          recovered matrix all three services publish an event @c 0x0001.
 */
using NodeEventHandler = std::function<void(ServiceId service, InstanceId instance, EventId notifier, ByteView payload)>;

/**
 * @brief One vsomeip application, hosting every service instance a node provides and consumes.
 */
class NodeRuntime final
{
  public:
    /**
     * @brief Build the runtime and bind it to the vsomeip application.
     * @param[in] appName Application name; must match an entry of the generated vsomeip configuration.
     * @return The runtime, or an error describing why it could not be built.
     * @retval ::ids::ecu::EcuErrc::InvalidConfiguration    @p appName is empty.
     * @retval ::ids::ecu::EcuErrc::ApplicationCreateFailed The vsomeip runtime refused to create the application.
     * @retval ::ids::ecu::EcuErrc::ApplicationInitFailed   @c init() refused; almost always a configuration
     *                                                     that does not name this application.
     * @details Exactly one of these per process. Creating a second with the same name is the
     *          silent-rename trap described in the file header.
     */
    static ara::core::Result<std::unique_ptr<NodeRuntime>> Create(const std::string &appName);

    /**
     * @brief Stop the stack and release the application.
     * @return None.
     */
    ~NodeRuntime();

    /**
     * @brief Declare a service instance this node will offer.
     * @param[in] service  Service to offer.
     * @param[in] instance Instance of it.
     * @return Nothing on success, or an error.
     * @retval ::ids::ecu::EcuErrc::InvalidConfiguration   An identifier is zero.
     * @retval ::ids::ecu::EcuErrc::AlreadyStarted         Called after Start().
     * @retval ::ids::ecu::EcuErrc::ServiceAlreadyDeclared The same instance was already declared.
     * @details Declares only; the announcement happens in Offer().
     */
    ara::core::Result<void> ProvideService(ServiceId service, InstanceId instance);

    /**
     * @brief Declare an event of a provided service.
     * @param[in] service     Service the event belongs to; must have been declared by ProvideService().
     * @param[in] instance    Instance of it.
     * @param[in] notifier    Event or field identifier.
     * @param[in] eventgroups Eventgroups the event belongs to; must not be empty.
     * @param[in] isField     Whether the matrix calls it a field, which keeps its last value for
     *                        late subscribers.
     * @param[in] cycle       Cyclic update period; zero means the event is only sent on demand.
     * @param[in] transport   Which endpoint the notifications travel on.
     * @return Nothing on success, or an error.
     * @retval ::ids::ecu::EcuErrc::InvalidConfiguration @p notifier is zero or @p eventgroups is empty.
     * @retval ::ids::ecu::EcuErrc::AlreadyStarted       Called after Start().
     * @retval ::ids::ecu::EcuErrc::ServiceNotDeclared   @p service and @p instance were not provided.
     * @warning Call this BEFORE Offer(). An event declared after the service is offered is not
     *          announced to subscribers that are already there.
     * @warning @p transport has NO DEFAULT on purpose. A service that announces both a reliable and
     *          an unreliable endpoint leaves vsomeip to choose, and over the nine reproduced nodes
     *          that choice gave 65% TCP against the 99,1% of the reference dataset. See
     *          ::ids::ecu::Transport.
     */
    ara::core::Result<void> ProvideEvent(ServiceId service, InstanceId instance, EventId notifier, EventgroupIds eventgroups, bool isField,
                                         std::chrono::milliseconds cycle, Transport transport);

    /**
     * @brief Declare a service instance this node will consume.
     * @param[in] service  Service to look for.
     * @param[in] instance Instance of it.
     * @return Nothing on success, or an error.
     * @retval ::ids::ecu::EcuErrc::InvalidConfiguration   An identifier is zero.
     * @retval ::ids::ecu::EcuErrc::AlreadyStarted         Called after Start().
     * @retval ::ids::ecu::EcuErrc::ServiceAlreadyDeclared The same instance was already declared.
     * @details Must run before Start(), because Start() is what requests every declared service,
     *          and Service Discovery only looks for what has been requested.
     */
    ara::core::Result<void> ConsumeService(ServiceId service, InstanceId instance);

    /**
     * @brief Install the handler that receives events of every consumed service.
     * @param[in] handler Callback invoked for each event that arrives; pass an empty function to remove it.
     * @return None.
     * @warning Set this BEFORE Subscribe(), otherwise the first notifications arrive with nowhere
     *          to go and are dropped.
     */
    void SetReceiveHandler(NodeEventHandler handler);

    /**
     * @brief Start the stack on its own thread and request every consumed service.
     * @return Nothing on success, or an error.
     * @retval ::ids::ecu::EcuErrc::AlreadyStarted Start() was already called.
     * @details Does not block. @c application::start() runs the event loop and never returns, so it
     *          goes on a thread of its own.
     */
    ara::core::Result<void> Start();

    /**
     * @brief Block until the routing manager has registered this application.
     * @param[in] timeout How long to wait.
     * @return Nothing once registered, or an error.
     * @retval ::ids::ecu::EcuErrc::NotStarted          Start() was not called.
     * @retval ::ids::ecu::EcuErrc::RegistrationTimeout Registration did not arrive within @p timeout.
     */
    ara::core::Result<void> WaitUntilRegistered(std::chrono::milliseconds timeout);

    /**
     * @brief Offer every declared provided service.
     * @return Nothing on success, or an error.
     * @retval ::ids::ecu::EcuErrc::NotStarted Start() was not called.
     * @details Records the intent and offers as soon as the application is registered, so the caller
     *          need not know that vsomeip ignores an earlier offer. A node that provides nothing may
     *          call it; it then does nothing.
     */
    ara::core::Result<void> Offer();

    /**
     * @brief Withdraw every offered service.
     * @return None.
     */
    void StopOffer() noexcept;

    /**
     * @brief Block until Service Discovery reports a consumed service as available.
     * @param[in] service  Service to wait for; must have been declared by ConsumeService().
     * @param[in] instance Instance of it.
     * @param[in] timeout  How long to wait.
     * @return Nothing once available, or an error.
     * @retval ::ids::ecu::EcuErrc::NotStarted                Start() was not called.
     * @retval ::ids::ecu::EcuErrc::ServiceNotDeclared        The instance was not consumed.
     * @retval ::ids::ecu::EcuErrc::ServiceUnavailableTimeout It was not announced within @p timeout.
     * @details A timeout here, with registration already done, is the signature of the provider not
     *          being announced: either it is not running, or the multicast route is missing on one
     *          of the two nodes. See platform/sd-smoke/README.md.
     */
    ara::core::Result<void> WaitUntilAvailable(ServiceId service, InstanceId instance, std::chrono::milliseconds timeout);

    /**
     * @brief Subscribe to an event of a consumed service.
     * @param[in] service     Service that publishes it; must have been declared by ConsumeService().
     * @param[in] instance    Instance of it.
     * @param[in] notifier    Event or field identifier to receive.
     * @param[in] eventgroups Eventgroups to subscribe to; must not be empty.
     * @param[in] transport   Which endpoint the notifications are expected on; must match what the
     *                        provider offered, and has no default for the same reason.
     * @return Nothing on success, or an error.
     * @retval ::ids::ecu::EcuErrc::NotStarted           Start() was not called.
     * @retval ::ids::ecu::EcuErrc::InvalidConfiguration @p notifier is zero or @p eventgroups is empty.
     * @retval ::ids::ecu::EcuErrc::ServiceNotDeclared   The instance was not consumed.
     * @details Call it after WaitUntilAvailable(), and after SetReceiveHandler().
     */
    ara::core::Result<void> Subscribe(ServiceId service, InstanceId instance, EventId notifier, EventgroupIds eventgroups,
                                      Transport transport);

    /**
     * @brief Publish a value on an event of a provided service.
     * @param[in] service  Service that owns the event.
     * @param[in] instance Instance of it.
     * @param[in] notifier Event identifier, which must have been declared by ProvideEvent().
     * @param[in] payload  Bytes to publish. Copied before returning.
     * @return Nothing on success, or an error.
     * @retval ::ids::ecu::EcuErrc::NotStarted      Start() was not called.
     * @retval ::ids::ecu::EcuErrc::EventNotOffered @p notifier was not declared for that instance.
     * @retval ::ids::ecu::EcuErrc::PayloadTooLarge @p payload exceeds ::ids::ecu::kMaxPayloadBytes.
     */
    ara::core::Result<void> Send(ServiceId service, InstanceId instance, EventId notifier, ByteView payload);

    /**
     * @brief Whether the routing manager has registered this application.
     * @return Whether the stack is registered.
     * @retval true  Registered.
     * @retval false Not registered.
     */
    bool IsRegistered() const noexcept;

    /**
     * @brief Whether Service Discovery currently reports a consumed service as available.
     * @param[in] service  Service to ask about.
     * @param[in] instance Instance of it.
     * @return Whether the remote service was found.
     * @retval true  Available.
     * @retval false Not available, or not a consumed instance.
     */
    bool IsAvailable(ServiceId service, InstanceId instance) const noexcept;

    /**
     * @brief Copy construction is deleted: a runtime owns a stack binding and a thread.
     * @return None.
     */
    NodeRuntime(const NodeRuntime &) = delete;

    /**
     * @brief Copy assignment is deleted: a runtime owns a stack binding and a thread.
     * @return Nothing; the operator cannot be called.
     */
    NodeRuntime &operator=(const NodeRuntime &) = delete;

  private:
    struct Impl; ///< vsomeip application, thread and per-instance state, kept out of this header.

    /**
     * @brief Construct from an already initialised application.
     * @param[in,out] impl Initialised state, taken over by this runtime.
     * @return None.
     * @details Private so that Create() is the only way in.
     */
    explicit NodeRuntime(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl; ///< Owned stack binding.
};

} // namespace ecu
} // namespace ids

/** @}*/ // End of LibEcuNodeRuntime
