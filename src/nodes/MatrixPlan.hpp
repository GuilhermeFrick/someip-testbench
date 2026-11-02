/**
 * @file      MatrixPlan.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     What the communication matrix says one node must do, as a value one can assert on.
 * @version   1.0
 * @date      2026-11-01
 * @copyright Copyright (c) 2026
 *
 * @details   The generic node does two separable things: it DERIVES, from the matrix, which events
 *            it must publish and which it must subscribe to; and it then runs a vsomeip stack doing
 *            exactly that. Only the second half needs a bus. This header is the first half, kept
 *            apart precisely so it can be asserted on without one.
 *
 * @details   That seam is not decoration. The eventgroup correlation of tools/extract_matrix.py was
 *            wrong once in a way that looked right: using "the destinations of the event are a
 *            SUBSET of the subscribers of the group" always matches when an event has a single
 *            destination, and the symptom was four eventgroups for four events, which is what a
 *            correct answer also looks like. The lesson is that eventgroup handling has to be
 *            checked against a known matrix instead of eyeballed, and a plan that is a plain value
 *            is what makes that possible.
 *
 * @details   Header-only and allocation-happy on purpose: this runs once, at start-up, before the
 *            stack is up, and never on the publishing path.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "build/cfg/CommMatrixTable.hpp"
#include "src/libecu/Types.hpp"

/** @defgroup Nodes Nodes
 *  @brief Executables that take the place of ECUs on the bench.
 */

/** @addtogroup NodesMatrixPlan MatrixPlan
 *  @ingroup Nodes
 *  @{
 */

namespace ids
{
namespace nodes
{

/**
 * @brief One event this node publishes, as the matrix describes it.
 */
struct EventPlan
{
    ::ids::ecu::ServiceId                     service  = 0U;    ///< Service that owns the event.
    ::ids::ecu::InstanceId                    instance = 0U;    ///< Instance of that service.
    ::ids::ecu::EventId                       notifier = 0U;    ///< Event identifier.
    bool                                      isField  = false; ///< Whether the matrix calls it a field.
    std::size_t                               bytes    = 0U;    ///< Payload size recovered from the capture.
    std::uint32_t                             cycleMs  = 0U;    ///< Cadence recovered from the capture.
    std::vector<::ids::ecu::EventgroupId>     eventgroups;      ///< Every group that announces it; never empty in a usable plan.
    std::string                               serviceName;      ///< Service name, for the log lines.
};

/**
 * @brief One service this node consumes, and which of its events it subscribes to.
 */
struct ConsumePlan
{
    ::ids::ecu::ServiceId                                                     service  = 0U; ///< Service to look for.
    ::ids::ecu::InstanceId                                                    instance = 0U; ///< Instance of it.
    std::map<::ids::ecu::EventId, std::vector<::ids::ecu::EventgroupId>>      subscriptions; ///< Event to the groups to subscribe it in.
    std::string                                                               serviceName;   ///< Service name, for the log lines.
};

/**
 * @brief Everything one node does, derived from the matrix.
 */
struct NodePlan
{
    const ::ids::matrix::table::NodeDesc *node = nullptr; ///< The node, or null when the matrix has no such name.
    std::vector<EventPlan>                publishes;      ///< Events to publish.
    std::vector<ConsumePlan>              consumes;       ///< Services to subscribe to.
    std::string                           problem;        ///< Empty when the plan is usable; otherwise what is wrong with the matrix.
};

/**
 * @brief Eventgroups of a service that carry a given event.
 * @param[in] service  Service name, as the matrix spells it.
 * @param[in] notifier Event to look for.
 * @return The identifiers of the groups that carry it, in matrix order; empty when none does.
 * @details An event in no group cannot be offered at all: vsomeip needs a group to announce it in.
 */
inline std::vector<::ids::ecu::EventgroupId> GroupsCarrying(const char *service, ::ids::ecu::EventId notifier)
{
    namespace tbl = ::ids::matrix::table;

    std::vector<::ids::ecu::EventgroupId> grupos;
    const tbl::Table<tbl::GroupDesc>      todos = tbl::GroupsOf(service);
    for (std::size_t g = 0U; g < todos.count; ++g)
    {
        bool carrega = false;
        for (std::size_t k = 0U; (k < todos.items[g].eventCount) && (!carrega); ++k)
        {
            carrega = (todos.items[g].events[k] == notifier);
        }
        if (carrega)
        {
            grupos.push_back(todos.items[g].id);
        }
        else
        {
            /* This group does not carry the event. */
        }
    }
    return grupos;
}

/**
 * @brief Which events a consumer must subscribe to, and in which groups.
 * @param[in] consume What the matrix says this node consumes of one service.
 * @return Map from event identifier to the groups to subscribe it in, each sorted and without repeats.
 * @details A @c groupCount of zero means "every eventgroup the service declares", which is the short
 *          form of a hand-written matrix. A recovered matrix always names the groups, because which
 *          ones a consumer subscribes to is TOPOLOGY: treating every consumer as if it subscribed to
 *          everything would make the reproduced traffic volume wrong, and that volume is the thing
 *          being reproduced.
 * @details An event that appears in two of the subscribed groups yields ONE entry with both groups,
 *          not two subscriptions of the same event.
 */
inline std::map<::ids::ecu::EventId, std::vector<::ids::ecu::EventgroupId>> SubscriptionsFor(const ::ids::matrix::table::ConsumeDesc &consume)
{
    namespace tbl = ::ids::matrix::table;

    std::map<::ids::ecu::EventId, std::set<::ids::ecu::EventgroupId>> juntando;

    const tbl::Table<tbl::GroupDesc> grupos = tbl::GroupsOf(consume.service);
    for (std::size_t g = 0U; g < grupos.count; ++g)
    {
        const tbl::GroupDesc &grupo = grupos.items[g];

        bool queremos = (consume.groupCount == 0U);
        for (std::size_t k = 0U; (k < consume.groupCount) && (!queremos); ++k)
        {
            queremos = (consume.eventgroups[k] == grupo.id);
        }

        if (queremos)
        {
            for (std::size_t k = 0U; k < grupo.eventCount; ++k)
            {
                juntando[grupo.events[k]].insert(grupo.id);
            }
        }
        else
        {
            /* A group this consumer does not subscribe to. */
        }
    }

    std::map<::ids::ecu::EventId, std::vector<::ids::ecu::EventgroupId>> saida;
    for (std::map<::ids::ecu::EventId, std::set<::ids::ecu::EventgroupId>>::const_iterator it = juntando.begin(); it != juntando.end();
         ++it)
    {
        saida[it->first] = std::vector<::ids::ecu::EventgroupId>(it->second.begin(), it->second.end());
    }
    return saida;
}

/**
 * @brief Derive the whole plan of one node from the matrix.
 * @param[in] nodeName Node name, as the matrix spells it.
 * @return The plan. @c node is null when there is no such node; @c problem names the first fault
 *         found when the matrix itself is inconsistent.
 * @details Reports the fault instead of returning an ara::core::Result because the faults here are
 *          not stack failures but statements about the matrix, and the node prints the sentence.
 *          Every one of them means the matrix and the generated table disagree, which
 *          tests/test_comm_matrix.cpp and tests/test_matrix_table.cpp exist to prevent; finding one
 *          here at start-up is the last line of defence, not the normal path.
 */
inline NodePlan PlanFor(const char *nodeName)
{
    namespace tbl = ::ids::matrix::table;

    NodePlan plano;
    plano.node = tbl::FindNode(nodeName);
    if (plano.node == nullptr)
    {
        plano.problem = std::string("a matriz nao tem no \"") + nodeName + "\"";
        return plano;
    }
    else
    {
        /* Found in the table. */
    }

    for (std::size_t i = 0U; i < plano.node->provideCount; ++i)
    {
        const char *const             svcNome = plano.node->provides[i];
        const tbl::ServiceDesc *const svc     = tbl::FindService(svcNome);
        if (svc == nullptr)
        {
            plano.problem = std::string("oferece \"") + svcNome + "\", que a matriz nao declara";
            return plano;
        }
        else
        {
            /* Declared. */
        }

        const tbl::Table<tbl::EventDesc> eventos = tbl::EventsOf(svcNome);
        for (std::size_t k = 0U; k < eventos.count; ++k)
        {
            const tbl::EventDesc &ev = eventos.items[k];

            EventPlan ep;
            ep.service     = svc->id;
            ep.instance    = svc->instance;
            ep.notifier    = ev.id;
            ep.isField     = ev.isField;
            ep.bytes       = ev.payloadBytes;
            ep.cycleMs     = ev.cycleMs;
            ep.eventgroups = GroupsCarrying(svcNome, ev.id);
            ep.serviceName = svcNome;

            if (ep.eventgroups.empty())
            {
                char texto[64];
                static_cast<void>(std::snprintf(texto, sizeof(texto), "0x%04X", static_cast<unsigned>(ev.id)));
                plano.problem = std::string("evento ") + texto + " de " + svcNome + " nao pertence a eventgroup algum";
                return plano;
            }
            else
            {
                plano.publishes.push_back(ep);
            }
        }
    }

    for (std::size_t i = 0U; i < plano.node->consumeCount; ++i)
    {
        const tbl::ConsumeDesc       &c   = plano.node->consumes[i];
        const tbl::ServiceDesc *const svc = tbl::FindService(c.service);
        if (svc == nullptr)
        {
            plano.problem = std::string("consome \"") + c.service + "\", que a matriz nao declara";
            return plano;
        }
        else
        {
            /* Declared. */
        }

        ConsumePlan cp;
        cp.service       = svc->id;
        cp.instance      = svc->instance;
        cp.serviceName   = c.service;
        cp.subscriptions = SubscriptionsFor(c);

        if (cp.subscriptions.empty())
        {
            plano.problem = std::string("consome \"") + c.service + "\" sem assinar evento algum";
            return plano;
        }
        else
        {
            plano.consumes.push_back(cp);
        }
    }

    return plano;
}

} // namespace nodes
} // namespace ids

/** @}*/ // End of NodesMatrixPlan
