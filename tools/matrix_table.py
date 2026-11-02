#!/usr/bin/env python3
"""Gera a tabela de RUNTIME da matriz de comunicação, para o nó genérico.

O `CommMatrix.hpp` dá os identificadores como funções `constexpr`, uma por
serviço e por nó. Isso serve a um nó escrito à mão para uma matriz específica —
é o que o `gnss_ecu` faz.

Um nó **genérico** precisa do contrário: percorrer, em tempo de execução, "os
serviços que este nó oferece" e "os eventos de cada um". Nove nós escritos à mão
para reproduzir um barramento de nove ECUs seriam nove arquivos que divergem da
matriz no primeiro descuido; um nó só, dirigido pela tabela, não pode divergir.

Tudo vive em `static` dentro de funções `inline`, que é o que mantém uma única
instância por programa sem `inline variable` — que C++14 não tem. É a mesma
restrição que já ditou as funções `constexpr` do outro cabeçalho, e que o
`tests/test_comm_matrix.cpp` cobre com a prova de ODR.

Importado por `tools/gen_config.py`; não roda sozinho.
"""
from __future__ import annotations


def consumos_de(node: dict) -> dict:
    """Serviços que um nó consome, e quais eventgroups de cada um.

    Duas formas são aceitas. A curta, escrita à mão:

        consumes: [GnssService]          # assina TODOS os eventgroups

    E a completa, que o `tools/extract_matrix.py` emite ao recuperar de uma
    captura, porque ali a assinatura é topologia e não detalhe — consumidores
    diferentes assinam eventgroups diferentes, e tratar todos como se
    assinassem tudo faria o volume de tráfego da reprodução sair errado:

        consumes:
          - { service: Service1001, eventgroups: [0x0001] }

    Devolve `{nome_do_servico: [eventgroups] ou None}`, onde `None` significa
    "todos os que o serviço declara".
    """
    saida = {}
    for item in (node.get("consumes") or []):
        if isinstance(item, dict):
            grupos = [int(g) for g in (item.get("eventgroups") or [])]
            saida[item["service"]] = grupos or None
        else:
            saida[str(item)] = None
    return saida


def _cabecalho() -> list:
    return [
        "/**",
        " * @file      CommMatrixTable.hpp",
        " * @author    tools/gen_config.py",
        " * @brief     Communication matrix as RUNTIME tables. GENERATED FILE.",
        " * @version   1.0",
        " * @copyright Copyright (c) 2026",
        " *",
        " * @details   DO NOT EDIT. Generated from the communication matrix by",
        " *            tools/gen_config.py, through tools/matrix_table.py.",
        " *",
        " * @details   CommMatrix.hpp gives the identifiers as constexpr functions, which suits a",
        " *            node written by hand for one matrix. A GENERIC node needs the opposite: to",
        " *            walk, at run time, the services it provides and the events of each. Nine",
        " *            hand-written nodes to reproduce a nine-ECU bus would be nine files that drift",
        " *            from the matrix at the first slip; one node driven by this table cannot drift.",
        " *",
        " * @details   Everything lives in statics inside inline functions, which is what keeps one",
        " *            instance per program without an inline variable, which C++14 does not have.",
        " *            The same restriction made the other header use constexpr functions.",
        " */",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <cstring>",
        "",
        '#include "src/libecu/Types.hpp"',
        "",
        "namespace ids",
        "{",
        "namespace matrix",
        "{",
        "namespace table",
        "{",
        "",
        "/** @brief One event, as the matrix declares it. */",
        "struct EventDesc",
        "{",
        "    ::ids::ecu::EventId id;           ///< Event identifier.",
        "    bool                isField;      ///< Whether the matrix calls it a field.",
        "    std::uint32_t       cycleMs;      ///< Publishing period; zero means on change only.",
        "    std::size_t         payloadBytes; ///< Payload size observed or declared, in bytes.",
        "};",
        "",
        "/** @brief One eventgroup and the events it carries. */",
        "struct GroupDesc",
        "{",
        "    ::ids::ecu::EventgroupId   id;         ///< Eventgroup identifier.",
        "    const ::ids::ecu::EventId *events;     ///< Events in the group.",
        "    std::size_t                eventCount; ///< How many.",
        "};",
        "",
        "/** @brief One service, with its endpoints. */",
        "struct ServiceDesc",
        "{",
        "    const char            *name;     ///< Name in the matrix.",
        "    ::ids::ecu::ServiceId  id;       ///< Service identifier.",
        "    ::ids::ecu::InstanceId instance; ///< Instance identifier.",
        "    bool                   hasTcp;   ///< Whether a reliable endpoint is declared.",
        "    ::ids::ecu::Port       tcpPort;  ///< Reliable port, when there is one.",
        "    bool                   hasUdp;   ///< Whether an unreliable endpoint is declared.",
        "    ::ids::ecu::Port       udpPort;  ///< Unreliable port, when there is one.",
        "    ::ids::ecu::Transport  transport; ///< Endpoint the events of this service travel on.",
        "};",
        "",
        "/** @brief What one node consumes: a service and the eventgroups it subscribes to. */",
        "struct ConsumeDesc",
        "{",
        "    const char                     *service;     ///< Name of the consumed service.",
        "    const ::ids::ecu::EventgroupId *eventgroups; ///< Groups subscribed; null means all.",
        "    std::size_t                     groupCount;  ///< How many; zero means all.",
        "};",
        "",
        "/** @brief One node of the bench. */",
        "struct NodeDesc",
        "{",
        "    const char               *name;         ///< Name in the matrix.",
        "    const char               *appName;      ///< vsomeip application name.",
        "    const char               *ip;           ///< Fixed address; labelling is by source address.",
        "    ::ids::ecu::ApplicationId appId;        ///< Application identifier.",
        "    const char *const        *provides;     ///< Services offered, by name.",
        "    std::size_t               provideCount; ///< How many.",
        "    const ConsumeDesc        *consumes;     ///< Services consumed.",
        "    std::size_t               consumeCount; ///< How many.",
        "};",
        "",
        "/** @brief A view over a generated table. */",
        "template <typename T>",
        "struct Table",
        "{",
        "    const T    *items; ///< First item, or null when the table is empty.",
        "    std::size_t count; ///< How many items.",
        "};",
        "",
    ]


def _por_servico(matrix: dict, endpoints_de, transporte_de) -> list:
    L = []
    for nome, svc in matrix["services"].items():
        por_nome = {e["name"]: e for e in svc["events"]}

        L.append("/** @brief Events of %s. */" % nome)
        L.append("inline Table<EventDesc> EventsOf%s() noexcept" % nome)
        L.append("{")
        L.append("    static const EventDesc items[] = {")
        for e in svc["events"]:
            L.append("        { 0x%04XU, %s, %dU, %dU },"
                     % (e["id"],
                        "true" if e.get("is_field") else "false",
                        int(round(float(e.get("cycle_ms") or 0))),
                        int(e.get("payload_bytes") or 8)))
        L.append("    };")
        L.append("    return Table<EventDesc>{ items, %d };" % len(svc["events"]))
        L.append("}")
        L.append("")

        grupos = svc.get("eventgroups") or []
        L.append("/** @brief Eventgroups of %s. */" % nome)
        L.append("inline Table<GroupDesc> GroupsOf%s() noexcept" % nome)
        L.append("{")
        if not grupos:
            L.append("    return Table<GroupDesc>{ nullptr, 0 };")
        else:
            for k, g in enumerate(grupos):
                ids_ev = ", ".join("0x%04XU" % por_nome[n]["id"] for n in g["events"])
                L.append("    static const ::ids::ecu::EventId g%d[] = { %s };" % (k, ids_ev))
            L.append("    static const GroupDesc items[] = {")
            for k, g in enumerate(grupos):
                L.append("        { 0x%04XU, g%d, %d }," % (g["id"], k, len(g["events"])))
            L.append("    };")
            L.append("    return Table<GroupDesc>{ items, %d };" % len(grupos))
        L.append("}")
        L.append("")

    nomes = list(matrix["services"].keys())

    L.append("/** @brief Every service the matrix declares. */")
    L.append("inline Table<ServiceDesc> Services() noexcept")
    L.append("{")
    L.append("    static const ServiceDesc items[] = {")
    for nome in nomes:
        svc = matrix["services"][nome]
        eps = endpoints_de(svc)
        L.append('        { "%s", 0x%04XU, 0x%04XU, %s, %dU, %s, %dU, ::ids::ecu::Transport::%s },'
                 % (nome, svc["id"], svc["instance"],
                    "true" if "tcp" in eps else "false", eps.get("tcp", 0),
                    "true" if "udp" in eps else "false", eps.get("udp", 0),
                    transporte_de(svc)))
    L.append("    };")
    L.append("    return Table<ServiceDesc>{ items, %d };" % len(nomes))
    L.append("}")
    L.append("")

    for rotulo, sufixo in (("Events", "EventDesc"), ("Groups", "GroupDesc")):
        L.append("/**")
        L.append(" * @brief %s of a service, by name." % rotulo)
        L.append(" * @param[in] service Service name, as the matrix spells it.")
        L.append(" * @return The table, or an empty one when the name is unknown.")
        L.append(" */")
        L.append("inline Table<%s> %sOf(const char *service) noexcept" % (sufixo, rotulo))
        L.append("{")
        for nome in nomes:
            L.append('    if (std::strcmp(service, "%s") == 0)' % nome)
            L.append("    {")
            L.append("        return %sOf%s();" % (rotulo, nome))
            L.append("    }")
        L.append("    return Table<%s>{ nullptr, 0 };" % sufixo)
        L.append("}")
        L.append("")
    return L


def _por_no(matrix: dict) -> list:
    L = []
    L.append("/** @brief Every node the matrix declares. */")
    L.append("inline Table<NodeDesc> Nodes() noexcept")
    L.append("{")
    for node_name, node in matrix["nodes"].items():
        provides = node.get("provides") or []
        consumes = consumos_de(node)
        if provides:
            L.append("    static const char *const p_%s[] = { %s };"
                     % (node_name, ", ".join('"%s"' % x for x in provides)))
        for svc_nome, grupos in consumes.items():
            if grupos:
                L.append("    static const ::ids::ecu::EventgroupId g_%s_%s[] = { %s };"
                         % (node_name, svc_nome, ", ".join("0x%04XU" % g for g in grupos)))
        if consumes:
            partes = []
            for svc_nome, grupos in consumes.items():
                if grupos:
                    partes.append('{ "%s", g_%s_%s, %d }' % (svc_nome, node_name, svc_nome, len(grupos)))
                else:
                    partes.append('{ "%s", nullptr, 0 }' % svc_nome)
            L.append("    static const ConsumeDesc c_%s[] = { %s };" % (node_name, ", ".join(partes)))
    L.append("    static const NodeDesc items[] = {")
    for node_name, node in matrix["nodes"].items():
        provides = node.get("provides") or []
        consumes = consumos_de(node)
        pv = ("p_%s, %d" % (node_name, len(provides))) if provides else "nullptr, 0"
        cs = ("c_%s, %d" % (node_name, len(consumes))) if consumes else "nullptr, 0"
        L.append('        { "%s", "%s", "%s", 0x%04XU, %s, %s },'
                 % (node_name, node["app_name"], node["ip"], node["app_id"], pv, cs))
    L.append("    };")
    L.append("    return Table<NodeDesc>{ items, %d };" % len(matrix["nodes"]))
    L.append("}")
    L.append("")

    for rotulo, tipo in (("Node", "NodeDesc"), ("Service", "ServiceDesc")):
        plural = "Nodes" if rotulo == "Node" else "Services"
        L.append("/**")
        L.append(" * @brief Look a %s up by name." % rotulo.lower())
        L.append(" * @param[in] name Name, as the matrix spells it.")
        L.append(" * @return Pointer to its description, or null when there is no such %s." % rotulo.lower())
        L.append(" */")
        L.append("inline const %s *Find%s(const char *name) noexcept" % (tipo, rotulo))
        L.append("{")
        L.append("    const Table<%s> todos = %s();" % (tipo, plural))
        L.append("    for (std::size_t i = 0U; i < todos.count; ++i)")
        L.append("    {")
        L.append("        if (std::strcmp(todos.items[i].name, name) == 0)")
        L.append("        {")
        L.append("            return &todos.items[i];")
        L.append("        }")
        L.append("    }")
        L.append("    return nullptr;")
        L.append("}")
        L.append("")

    L.append("} // namespace table")
    L.append("} // namespace matrix")
    L.append("} // namespace ids")
    L.append("")
    return L


def tabela_lines(matrix: dict, endpoints_de, transporte_de) -> list:
    """Monta o CommMatrixTable.hpp inteiro.

    `endpoints_de` e `transporte_de` vêm do gen_config, para as regras de "qual
    porta em qual transporte" e "em qual endpoint os eventos viajam" existirem
    num lugar só.

    Um serviço que anuncia os dois endpoints e não declara `protocol:` sai como
    `Unspecified`, e isso é AVISADO em voz alta: foi deixando o transporte
    implícito que a reprodução dos nove nós deu 65% de TCP contra os 99,1% do
    dataset de referência.
    """
    indefinidos = [nome for nome, svc in matrix["services"].items()
                   if transporte_de(svc) == "Unspecified"]
    if indefinidos:
        print("  AVISO: transporte indefinido em " + ", ".join(indefinidos))
        print("         Os eventos sairao com RT_UNKNOWN e o vsomeip escolhe o endpoint.")
        print("         Declare `protocol: tcp` (ou udp) no servico para fixar.")
    return _cabecalho() + _por_servico(matrix, endpoints_de, transporte_de) + _por_no(matrix)
