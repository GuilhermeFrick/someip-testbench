#!/usr/bin/env python3
"""Security events de uma rodada viva, lidos contra a linha do tempo do ataque.

Fase B de docs/PLANO.md: o ataque sai no fio, o sensor classifica ao vivo, e os
security events têm de aparecer DENTRO da janela de ataque. Este script lê o sink
do IdsM (uma linha por evento, com o carimbo do quadro em µs e o contexto
"IP de origem | serviço | método | confiança") e a linha do tempo da rodada
(platform/scenario-live/run.sh grava T0_EPOCH e o instante em que cada injetor
sobe) e responde:

  * quantos eventos, por classe, antes e depois da entrada do primeiro injetor;
  * quanto tempo depois da entrada saiu o primeiro evento de classe de ataque;
  * de quais origens vêm os eventos (as dos injetores, as das ECUs legítimas).

Uso:
    python3 tools/events_timeline.py --events out-dos/security-events.txt \\
            --timeline out-dos/linha-do-tempo.txt
"""
import argparse
import socket
import struct
from collections import Counter


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--events", required=True)
    ap.add_argument("--timeline", required=True)
    a = ap.parse_args()

    t0 = None
    entrada = None
    injetores = []
    with open(a.timeline, encoding="utf-8") as fh:
        for linha in fh:
            if linha.startswith("T0_EPOCH="):
                t0 = float(linha.split("=", 1)[1])
            elif "sobe injetor" in linha:
                rel = float(linha.split()[0].replace(",", "."))
                injetores.append(linha.strip())
                entrada = rel if entrada is None else min(entrada, rel)
    if t0 is None:
        raise SystemExit("ERRO: a linha do tempo não tem T0_EPOCH")

    eventos = []
    with open(a.events, encoding="utf-8") as fh:
        for linha in fh:
            campos = dict(x.split("=", 1) for x in linha.split() if "=" in x)
            if "context" not in campos or campos.get("ts", "-") == "-":
                continue
            ctx = bytes.fromhex(campos["context"])
            if len(ctx) != 12:
                continue
            t = int(campos["ts"]) / 1e6 - t0
            classe = campos["path"].rsplit("/", 1)[-1]
            origem = socket.inet_ntoa(ctx[0:4])
            svc, met = struct.unpack(">HH", ctx[4:8])
            (conf,) = struct.unpack(">f", ctx[8:12])
            eventos.append((t, classe, origem, svc, met, conf))
    eventos.sort()

    print("injetores:")
    for linha in injetores:
        print("  " + linha)
    if not eventos:
        print("nenhum security event no sink")
        return
    antes = Counter(c for t, c, *_ in eventos if entrada is not None and t < entrada)
    depois = Counter(c for t, c, *_ in eventos if entrada is None or t >= entrada)
    print("security events: %d (de %.1f s a %.1f s após T0)" % (len(eventos), eventos[0][0], eventos[-1][0]))
    print("  %-24s %10s %10s" % ("classe", "antes", "depois"))
    for c in sorted(set(antes) | set(depois)):
        print("  %-24s %10d %10d" % (c, antes[c], depois[c]))
    if entrada is not None:
        primeiro = next((e for e in eventos if e[0] >= entrada), None)
        if primeiro:
            print("primeiro evento depois da entrada do injetor: +%.3f s (%s, origem %s, svc 0x%04X, confiança %.3f)"
                  % (primeiro[0] - entrada, primeiro[1], primeiro[2], primeiro[3], primeiro[5]))
    print("por origem e classe (depois da entrada):")
    por = Counter((o, c) for t, c, o, *_ in eventos if entrada is None or t >= entrada)
    for (o, c), k in por.most_common(12):
        print("  %-15s %-24s %8d" % (o, c, k))


if __name__ == "__main__":
    main()
