#!/usr/bin/env python3
"""Gera a configuração de cada nó a partir da matriz de comunicação.

A matriz (platform/comm_matrix.yaml) é a fonte única da bancada. Deste arquivo
sai a configuração da pilha vsomeip de cada nó, o mapa de rótulos do PCAP e os
cabeçalhos C++ com os identificadores, de modo que pilha, código e rotulagem não
possam divergir.

O cabeçalho C++ é o que impede o identificador de virar literal espalhado pelo
código dos nós. Um `0x1234` escrito à mão compila igual, e só se descobre errado
quando o Service Discovery não fecha.

Validação: como os identificadores da matriz coincidem com os dos exemplos do
vsomeip, a config gerada pode ser testada contra binários conhecidamente
corretos (ver platform/sd-smoke).

Uso:
    python tools/gen_config.py --matrix platform/comm_matrix.yaml --out build/cfg
"""
from __future__ import annotations

import argparse
import json
import os
import re

import yaml


def hex16(v) -> str:
    """vsomeip espera os identificadores como string hexadecimal."""
    return v if isinstance(v, str) else f"0x{v:04x}"


def sd_block(net: dict) -> dict:
    sd = net["service_discovery"]
    return {
        "enable": "true",
        "multicast": sd["multicast"],
        "port": str(sd["port"]),
        "protocol": sd["protocol"],
        "initial_delay_min": str(sd["initial_delay_min"]),
        "initial_delay_max": str(sd["initial_delay_max"]),
        "repetitions_base_delay": str(sd["repetitions_base_delay"]),
        "repetitions_max": str(sd["repetitions_max"]),
        "ttl": str(sd["ttl"]),
        "cyclic_offer_delay": str(sd["cyclic_offer_delay"]),
    }


def service_block(svc: dict) -> dict:
    """Traduz um serviço da matriz para o formato do vsomeip."""
    by_name = {e["name"]: e for e in svc["events"]}

    events = []
    for e in svc["events"]:
        ev = {"event": hex16(e["id"])}
        if e.get("is_field"):
            ev["is_field"] = "true"
        if "cycle_ms" in e:
            ev["update-cycle"] = e["cycle_ms"]
        events.append(ev)

    groups = []
    for g in svc.get("eventgroups", []):
        grp = {
            "eventgroup": hex16(g["id"]),
            "events": [hex16(by_name[n]["id"]) for n in g["events"]],
        }
        if "multicast" in g:
            m = g["multicast"]
            grp["multicast"] = {"address": m["address"], "port": str(m["port"])}
            # `threshold` so e emitido quando declarado: o vsomeip tem default
            # proprio, e inventar um valor mudaria o comportamento em silencio.
            if "threshold" in m:
                grp["threshold"] = str(m["threshold"])
        groups.append(grp)

    return {
        "service": hex16(svc["id"]),
        "instance": hex16(svc["instance"]),
        "unreliable": str(svc["port"]),
        "events": events,
        "eventgroups": groups,
    }


def pascal(name: str) -> str:
    """`gnss_ecu` -> `GnssEcu`. Serve para nomear o struct de cada nó."""
    return "".join(p.capitalize() for p in re.split(r"[_\-]", name) if p)


def hpp_lines(matrix: dict) -> list:
    """Gera o cabe\u00e7alho C++ com os identificadores da matriz.

    Os valores saem como FUN\u00c7\u00d5ES `constexpr`, n\u00e3o como membros de dado. Em C++14
    n\u00e3o existe `inline variable`: um `static constexpr` membro passado por
    refer\u00eancia constante \u00e9 odr-used e a liga\u00e7\u00e3o falha. Fun\u00e7\u00e3o `constexpr` \u00e9
    implicitamente inline, logo serve em qualquer uso, e continua avaliada em
    tempo de compila\u00e7\u00e3o. O tests/test_comm_matrix.cpp cobre exatamente esse caso.
    """
    L = []
    L.append("/**")
    L.append(" * @file      CommMatrix.hpp")
    L.append(" * @author    tools/gen_config.py")
    L.append(" * @brief     Identifiers from the communication matrix. GENERATED FILE.")
    L.append(" * @version   1.0")
    L.append(" * @copyright Copyright (c) 2026")
    L.append(" *")
    L.append(" * @details   DO NOT EDIT. Generated from platform/comm_matrix.yaml by")
    L.append(" *            tools/gen_config.py. Editing here makes the code diverge from the vsomeip")
    L.append(" *            stack and from the PCAP labelling, which come from the same source.")
    L.append(" *")
    L.append(" * @details   The values are constexpr FUNCTIONS, not data members. C++14 has no inline")
    L.append(" *            variable, and a static constexpr member passed by const reference is")
    L.append(" *            odr-used, which makes the link fail. A constexpr function is implicitly")
    L.append(" *            inline and works in every use.")
    L.append(" */")
    L.append("#pragma once")
    L.append("")
    L.append('#include "src/libecu/Types.hpp"')
    L.append("")
    L.append("/** @addtogroup LibEcuCommMatrix Communication matrix identifiers")
    L.append(" *  @ingroup LibEcu")
    L.append(" *  @{")
    L.append(" */")
    L.append("")
    L.append("namespace ids")
    L.append("{")
    L.append("namespace matrix")
    L.append("{")
    L.append("")

    def fn(tipo, nome, valor, doc):
        """Uma funcao constexpr de uma linha, documentada."""
        L.append("    /**")
        L.append("     * @brief %s" % doc)
        L.append("     * @return %s" % valor)
        L.append("     */")
        L.append("    static constexpr %s %s() noexcept" % (tipo, nome))
        L.append("    {")
        L.append("        return %s;" % valor)
        L.append("    }")
        L.append("")

    # ---------------- rede
    net = matrix["network"]
    sd = net["service_discovery"]
    L.append("/**")
    L.append(" * @brief Network parameters of the bench.")
    L.append(" */")
    L.append("struct Network final")
    L.append("{")
    fn("const char *", "Subnet", '"%s"' % net["subnet"], "Subnet the nodes live on.")
    fn("const char *", "MulticastRoute", '"%s"' % net["multicast_route"],
       "Route each node adds before starting the stack; without it vsomeip never enables SD.")
    fn("const char *", "SdMulticast", '"%s"' % sd["multicast"], "Multicast group of the Service Discovery.")
    fn("::ids::ecu::Port", "SdPort", "%dU" % sd["port"], "Port of the Service Discovery.")
    L.append("};")
    L.append("")

    # ---------------- servicos
    for svc_name, svc in matrix["services"].items():
        L.append("/**")
        L.append(" * @brief Service %s, as the matrix declares it." % svc_name)
        L.append(" */")
        L.append("struct %s final" % svc_name)
        L.append("{")
        fn("::ids::ecu::ServiceId", "Id", "0x%04XU" % svc["id"], "Service identifier.")
        fn("::ids::ecu::InstanceId", "Instance", "0x%04XU" % svc["instance"], "Instance identifier.")
        fn("::ids::ecu::Port", "UdpPort", "%dU" % svc["port"], "UDP port the service is offered on.")
        L.append("    /**")
        L.append("     * @brief Events the service offers.")
        L.append("     */")
        L.append("    struct Events final")
        L.append("    {")
        for e in svc["events"]:
            campo = "Field" if e.get("is_field") else "Event"
            ciclo = (", %d ms cycle" % e["cycle_ms"]) if e.get("cycle_ms") else ""
            L.append("        /**")
            L.append("         * @brief %s%s." % (campo, ciclo))
            L.append("         * @return 0x%04XU" % e["id"])
            L.append("         */")
            L.append("        static constexpr ::ids::ecu::EventId %s() noexcept" % pascal(e["name"]))
            L.append("        {")
            L.append("            return 0x%04XU;" % e["id"])
            L.append("        }")
            L.append("")
        L.append("    };")
        grupos = svc.get("eventgroups") or []
        if grupos:
            L.append("")
            L.append("    /**")
            L.append("     * @brief Eventgroups the service declares.")
            L.append("     */")
            L.append("    struct Eventgroups final")
            L.append("    {")
            for g in grupos:
                nome = pascal(g["name"]) if g.get("name") else ("Group%04X" % g["id"])
                membros = ", ".join(g["events"])
                mc = ""
                if "multicast" in g:
                    mc = " Has its own multicast address (%s:%d)." % (g["multicast"]["address"], g["multicast"]["port"])
                L.append("        /**")
                L.append("         * @brief Groups: %s.%s" % (membros, mc))
                L.append("         * @return 0x%04XU" % g["id"])
                L.append("         */")
                L.append("        static constexpr ::ids::ecu::EventgroupId %s() noexcept" % nome)
                L.append("        {")
                L.append("            return 0x%04XU;" % g["id"])
                L.append("        }")
                L.append("")
            L.append("    };")
        L.append("};")
        L.append("")

    # ---------------- nos
    for node_name, node in matrix["nodes"].items():
        L.append("/**")
        L.append(" * @brief Node %s (%s), as the matrix declares it." % (node_name, node.get("role", "")))
        L.append(" */")
        L.append("struct %s final" % pascal(node_name))
        L.append("{")
        fn("const char *", "Name", '"%s"' % node_name, "Name of the node in the matrix.")
        fn("const char *", "AppName", '"%s"' % node["app_name"], "Name of the vsomeip application.")
        fn("const char *", "Ip", '"%s"' % node["ip"],
           "Fixed address. It is a contract: PCAP labelling is by source address.")
        fn("::ids::ecu::ApplicationId", "AppId", "0x%04XU" % node["app_id"], "Application identifier.")
        L.append("};")
        L.append("")

    L.append("} // namespace matrix")
    L.append("} // namespace ids")
    L.append("")
    L.append("/** @}*/ // End of LibEcuCommMatrix")
    L.append("")
    return L


def node_config(name: str, node: dict, matrix: dict) -> dict:
    cfg = {
        "unicast": node["ip"],
        "logging": {"level": "info", "console": "true", "dlt": "false"},
        "applications": [{"name": node["app_name"], "id": hex16(node["app_id"])}],
        # Cada nó é autônomo: ele mesmo hospeda seu routing manager.
        "routing": node["app_name"],
        "service-discovery": sd_block(matrix["network"]),
    }
    provided = node.get("provides") or []
    if provided:
        cfg["services"] = [service_block(matrix["services"][s]) for s in provided]
    return cfg


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--matrix", default="platform/comm_matrix.yaml")
    ap.add_argument("--out", default="build/cfg")
    ap.add_argument("--out-hpp", default=None,
                    help="destino do cabecalho C++ (padrao: <out>/CommMatrix.hpp)")
    a = ap.parse_args()

    matrix = yaml.safe_load(open(a.matrix, encoding="utf-8"))
    os.makedirs(a.out, exist_ok=True)

    labels = {}
    for name, node in matrix["nodes"].items():
        cfg = node_config(name, node, matrix)
        path = os.path.join(a.out, f"vsomeip-{name}.json")
        json.dump(cfg, open(path, "w", encoding="utf-8"), indent=4)
        labels[node["ip"]] = {"node": name, "role": node["role"]}
        n_svc = len(cfg.get("services", []))
        print(f"  {name:12s} {node['ip']:14s} app={cfg['applications'][0]['id']} "
              f"servicos={n_svc}  -> vsomeip-{name}.json")

    # Mapa IP -> papel: é o que torna a rotulagem do PCAP possível, e sai da
    # mesma fonte que a configuração da pilha.
    lab_path = os.path.join(a.out, "labels.json")
    json.dump(labels, open(lab_path, "w", encoding="utf-8"), indent=4)
    print(f"\n  mapa de rotulos -> {os.path.basename(lab_path)} ({len(labels)} nos)")

    # Cabecalho C++: a mesma fonte que gera a pilha gera os identificadores do
    # codigo, para nao haver um literal escrito a mao em lugar nenhum.
    hpp_path = a.out_hpp or os.path.join(a.out, "CommMatrix.hpp")
    os.makedirs(os.path.dirname(os.path.abspath(hpp_path)), exist_ok=True)
    linhas = hpp_lines(matrix)
    with open(hpp_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(linhas))
    n_svc = len(matrix["services"])
    n_ev = sum(len(s["events"]) for s in matrix["services"].values())
    print(f"  cabecalho C++  -> {os.path.basename(hpp_path)} "
          f"({n_svc} servicos, {n_ev} eventos, {len(matrix['nodes'])} nos)")

    print(f"\n[ok] {os.path.abspath(a.out)}")


if __name__ == "__main__":
    main()
