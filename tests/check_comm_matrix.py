#!/usr/bin/env python3
"""Compara a saida do test_comm_matrix com a matriz de comunicacao.

Le o YAML de forma INDEPENDENTE do gerador, monta o que o cabecalho deveria
conter, e confere contra o que o binario imprimiu. Se o gerador trocar servico
por instancia, ou perder um eventgroup, a diferenca aparece aqui.

Uso:
    python tools/gen_config.py --out build/cfg
    g++ -std=c++14 -I. -o /tmp/t tests/test_comm_matrix.cpp && /tmp/t > /tmp/saida
    python tests/check_comm_matrix.py platform/comm_matrix.yaml /tmp/saida
"""
from __future__ import annotations

import sys

import yaml


def esperado(matrix: dict) -> dict:
    """Monta, a partir do YAML, o conjunto de pares que o cabecalho deve expor."""
    e = {}
    net = matrix["network"]
    sd = net["service_discovery"]
    e["network.subnet"] = net["subnet"]
    e["network.multicast_route"] = net["multicast_route"]
    e["network.sd_multicast"] = sd["multicast"]
    e["network.sd_port"] = str(sd["port"])

    for nome, svc in matrix["services"].items():
        e["service.%s.id" % nome] = str(svc["id"])
        e["service.%s.instance" % nome] = str(svc["instance"])
        e["service.%s.port" % nome] = str(svc["port"])
        for ev in svc["events"]:
            e["event.%s.%s" % (nome, ev["name"])] = str(ev["id"])
        for g in svc.get("eventgroups") or []:
            e["eventgroup.%s.%04x" % (nome, g["id"])] = str(g["id"])

    for nome, no in matrix["nodes"].items():
        e["node.%s.name" % nome] = nome
        e["node.%s.app_name" % nome] = no["app_name"]
        e["node.%s.ip" % nome] = no["ip"]
        e["node.%s.app_id" % nome] = str(no["app_id"])
    return e


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("uso: check_comm_matrix.py <comm_matrix.yaml> <saida-do-binario>")

    matrix = yaml.safe_load(open(sys.argv[1], encoding="utf-8"))
    esp = esperado(matrix)

    obtido = {}
    for linha in open(sys.argv[2], encoding="utf-8"):
        linha = linha.strip()
        if "=" in linha:
            k, v = linha.split("=", 1)
            obtido[k] = v

    faltando = sorted(set(esp) - set(obtido))
    sobrando = sorted(k for k in (set(obtido) - set(esp)) if not k.startswith("odr."))
    divergentes = sorted(k for k in (set(esp) & set(obtido)) if esp[k] != obtido[k])

    for k in faltando:
        print("  FALTA no cabecalho: %-36s esperado=%s" % (k, esp[k]))
    for k in sobrando:
        print("  SOBRA no cabecalho: %-36s obtido=%s" % (k, obtido[k]))
    for k in divergentes:
        print("  DIVERGE: %-36s yaml=%-18s cabecalho=%s" % (k, esp[k], obtido[k]))

    # As duas linhas de ODR tem de estar presentes: sao a prova de que o
    # cabecalho liga quando as constantes sao passadas por referencia.
    odr = [k for k in obtido if k.startswith("odr.")]
    if len(odr) != 2:
        print("  FALTA a prova de ODR (esperadas 2 linhas odr.*, vistas %d)" % len(odr))

    n = len(faltando) + len(sobrando) + len(divergentes) + (0 if len(odr) == 2 else 1)
    print()
    if n == 0:
        print("round-trip OK: %d identificadores conferidos contra o YAML, mais a prova de ODR" % len(esp))
    else:
        print("FALHOU: %d problemas" % n)
    raise SystemExit(0 if n == 0 else 1)


if __name__ == "__main__":
    main()
