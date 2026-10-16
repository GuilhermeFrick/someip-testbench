#!/usr/bin/env python3
"""Gera a configuração de cada nó a partir da matriz de comunicação.

A matriz (platform/comm_matrix.yaml) é a fonte única da bancada. Deste arquivo
sai a configuração da pilha vsomeip de cada nó e o mapa de rótulos do PCAP, de
modo que pilha e rotulagem não possam divergir.

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
    print(f"\n[ok] {os.path.abspath(a.out)}")


if __name__ == "__main__":
    main()
