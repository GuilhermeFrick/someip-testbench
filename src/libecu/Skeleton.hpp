/**
 * @file      Skeleton.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Provider side of the libecu facade: offers a service and publishes its events.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   The node writes against @c Skeleton and knows nothing about @c vsomeip::application,
 *            @c register_state_handler or the lifecycle of the routing manager. That is the point
 *            of the facade, and it is what makes a node comparable to real ECU code.
 *
 * @details   What the facade actually hides, and why it is not cosmetic:
 *            - **the offer must wait for registration.** vsomeip only accepts @c offer_service
 *              after the routing manager has registered the application, which it signals through
 *              a state handler. Calling Offer() earlier would silently do nothing. Here Offer()
 *              records the intent and the state handler performs it, so the caller does not have
 *              to know the ordering;
 *            - **@c application::start() blocks**, because it runs the event loop. Start() puts it
 *              on its own thread and WaitUntilRegistered() gives the caller a deterministic point
 *              to continue from, instead of a sleep;
 *            - **the multicast route trap.** Without @c "ip route add 224.0.0.0/4" the Service
 *              Discovery never announces the service, and vsomeip reports nothing: the offer just
 *              loops through @c pending_sd_offers forever. See platform/sd-smoke/README.md.
 *              ::ids::ecu::EcuErrc::ServiceDiscoveryNotReady turns that silence into an error the
 *              caller has to handle.
 */
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "src/ara/core/Result.hpp"
#include "src/libecu/EcuErrorDomain.hpp"
#include "src/libecu/Types.hpp"

/** @addtogroup LibEcuSkeleton Skeleton
 *  @ingroup LibEcu
 *  @{
 */

namespace ids
{
namespace ecu
{

/**
 * @brief What a skeleton needs in order to offer a service.
 * @details The values come from the generated build/cfg/CommMatrix.hpp, never written by hand.
 */
struct SkeletonConfig
{
    std::string appName;      ///< Application name; must match an entry of the generated vsomeip configuration.
    ServiceId   service  = 0U; ///< Service to offer.
    InstanceId  instance = 0U; ///< Instance of the service to offer.
};

/**
 * @brief Offers one service instance and publishes its events.
 */
class Skeleton final
{
  public:
    /**
     * @brief Build a skeleton and bind it to the vsomeip application.
     * @param[in] cfg Service to offer and the application name to bind to.
     * @return The skeleton, or an error describing why it could not be built.
     * @retval ::ids::ecu::EcuErrc::InvalidConfiguration    The application name is empty, or an
     *                                                     identifier is zero.
     * @retval ::ids::ecu::EcuErrc::ApplicationCreateFailed The vsomeip runtime refused to create
     *                                                     the application.
     * @retval ::ids::ecu::EcuErrc::ApplicationInitFailed   @c init() refused; almost always a
     *                                                     configuration that does not name this
     *                                                     application.
     * @details Returns a smart pointer, and the constructor is private, so a skeleton that exists
     *          is one whose application is initialised.
     */
    static ara::core::Result<std::unique_ptr<Skeleton>> Create(const SkeletonConfig &cfg);

    /**
     * @brief Stop the stack and release the application.
     * @return None.
     * @details Joins the event-loop thread, so the destructor returns only once the stack is down.
     */
    ~Skeleton();

    /**
     * @brief Declare an event, before the service is offered.
     * @param[in] notifier    Event or field identifier.
     * @param[in] eventgroups Eventgroups the event belongs to; must not be empty.
     * @param[in] isField     Whether this is a field, which keeps its last value for late
     *                        subscribers, rather than a plain event.
     * @param[in] cycle       Cyclic update period; zero means the event is only sent on demand.
     * @return Nothing on success, or an error.
     * @retval ::ids::ecu::EcuErrc::InvalidConfiguration @p notifier is zero or @p eventgroups is empty.
     * @warning Call this BEFORE Offer(). An event declared after the service is offered is not
     *          announced to subscribers that are already there.
     */
    ara::core::Result<void> OfferEvent(EventId notifier, EventgroupIds eventgroups, bool isField, std::chrono::milliseconds cycle);

    /**
     * @brief Start the stack on its own thread.
     * @return Nothing on success, or an error.
     * @retval ::ids::ecu::EcuErrc::AlreadyStarted Start() was already called on this object.
     * @details Does not block. @c vsomeip::application::start() runs the event loop and never
     *          returns until the stack is stopped, so it goes on a thread of its own.
     */
    ara::core::Result<void> Start();

    /**
     * @brief Block until the routing manager has registered this application.
     * @param[in] timeout How long to wait.
     * @return Nothing once registered, or an error on timeout.
     * @retval ::ids::ecu::EcuErrc::NotStarted          Start() was not called.
     * @retval ::ids::ecu::EcuErrc::RegistrationTimeout Registration did not arrive within @p timeout.
     * @details Gives the caller a deterministic point to continue from, instead of a sleep.
     */
    ara::core::Result<void> WaitUntilRegistered(std::chrono::milliseconds timeout);

    /**
     * @brief Offer the service instance.
     * @return Nothing on success, or an error.
     * @retval ::ids::ecu::EcuErrc::NotStarted Start() was not called.
     * @details Records the intent and offers as soon as the application is registered, so the
     *          caller need not know that vsomeip refuses an earlier offer. Calling it after
     *          registration offers immediately.
     */
    ara::core::Result<void> Offer();

    /**
     * @brief Withdraw the service instance.
     * @return None.
     */
    void StopOffer() noexcept;

    /**
     * @brief Publish a value on one of the declared events.
     * @param[in] notifier Event or field identifier, which must have been declared by OfferEvent().
     * @param[in] payload  Bytes to publish. Copied before returning, so it need not outlive the call.
     * @return Nothing on success, or an error.
     * @retval ::ids::ecu::EcuErrc::NotStarted      Start() was not called.
     * @retval ::ids::ecu::EcuErrc::EventNotOffered @p notifier was not declared by OfferEvent().
     * @retval ::ids::ecu::EcuErrc::PayloadTooLarge @p payload exceeds ::ids::ecu::kMaxPayloadBytes.
     */
    ara::core::Result<void> Send(EventId notifier, ByteView payload);

    /**
     * @brief Whether the routing manager has registered this application.
     * @return Whether the stack is registered and able to offer.
     * @retval true  Registered.
     * @retval false Not registered, either not yet or deregistered again.
     */
    bool IsRegistered() const noexcept;

    /**
     * @brief Copy construction is deleted: a skeleton owns a stack binding and a thread.
     * @return None.
     */
    Skeleton(const Skeleton &) = delete;

    /**
     * @brief Copy assignment is deleted: a skeleton owns a stack binding and a thread.
     * @return Nothing; the operator cannot be called.
     */
    Skeleton &operator=(const Skeleton &) = delete;

  private:
    struct Impl; ///< vsomeip application, thread and offer state, kept out of this header.

    /**
     * @brief Construct from an already initialised application.
     * @param[in,out] impl Initialised state, taken over by this skeleton.
     * @return None.
     * @details Private so that Create() is the only way in.
     */
    explicit Skeleton(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl; ///< Owned stack binding.
};

} // namespace ecu
} // namespace ids

/** @}*/ // End of LibEcuSkeleton
