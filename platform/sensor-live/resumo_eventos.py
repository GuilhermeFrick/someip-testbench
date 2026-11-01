#!/usr/bin/env python3
"""Abre o sink de Security Events por origem, serviço e confiança.

A contagem por classe sozinha engana. Num barramento real os eventos vêm de
origens diferentes por motivos diferentes, e separar isso é o que transforma "o
IDS detectou 33 coisas" em um diagnóstico.

O contexto é opaco para o `ara::idsm`, de propósito — o layout pertence ao
meta-modelo. Quem decodifica é quem conhece o layout, e é este script, que
espelha o `SecurityEventContext::Serialize()`:

    uint32 endereço de origem | uint16 service | uint16 method | float confiança

tudo em ordem de rede.

Uso:
    python3 platform/sensor-live/resumo_eventos.py platform/sensor-live/out/security-events.txt
"""
from __future__ import annotations

import collections
import socket
import struct
import sys

TAMANHO_CONTEXTO = 12


def main() -> None:
    if len(sys.argv) != 2:
        print("uso: resumo_eventos.py <security-events.txt>")
        raise SystemExit(2)

    por_classe = collections.Counter()
    por_origem = collections.Counter()
    por_origem_classe = collections.Counter()
    por_servico = collections.Counter()
    confiancas = []

    with open(sys.argv[1], encoding="utf-8") as fh:
        for linha in fh:
            campos = dict(x.split("=", 1) for x in linha.split() if "=" in x)
            if "context" not in campos or "path" not in campos:
                continue
            ctx = bytes.fromhex(campos["context"])
            if len(ctx) != TAMANHO_CONTEXTO:
                continue
            origem = socket.inet_ntoa(ctx[0:4])
            service, method = struct.unpack(">HH", ctx[4:8])
            (conf,) = struct.unpack(">f", ctx[8:12])
            classe = campos["path"].rsplit("/", 1)[-1]

            por_classe[classe] += 1
            por_origem[origem] += 1
            por_origem_classe[(origem, classe)] += 1
            por_servico[(origem, service, method)] += 1
            confiancas.append(conf)

    if not confiancas:
        print("  nenhum evento no sink")
        return

    print("  por classe:")
    for classe, n in por_classe.most_common():
        print("    %-24s %4d" % (classe, n))

    print("  por origem e serviço:")
    for (origem, service, method), n in sorted(por_servico.items(), key=lambda kv: -kv[1]):
        nota = ""
        if service == 0xFFFF:
            nota = "  <- Service Discovery"
        elif service == 0x0000:
            nota = "  <- NAO e SOME/IP: o parser aceita qualquer payload UDP de 16+ bytes"
        print("    %-15s svc=0x%04X met=0x%04X  %4d%s" % (origem, service, method, n, nota))

    confiancas.sort()
    meio = confiancas[len(confiancas) // 2]
    print("  confiança: min %.4f  mediana %.4f  max %.4f" % (confiancas[0], meio, confiancas[-1]))
    if meio < 0.7:
        print("    (mediana baixa: o modelo esta EXTRAPOLANDO, nao detectando com convicção.")
        print("     Uma cadeia de filtros do IdsM cortaria a maior parte disto, e e la que")
        print("     essa decisao pertence -- o sensor nao filtra, de proposito.)")


if __name__ == "__main__":
    main()
