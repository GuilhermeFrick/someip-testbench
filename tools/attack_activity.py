#!/usr/bin/env python3
"""Atividade de cada injetor no barramento, em janelas de tempo, com os comandos marcados.

Critério de aceite da fase D (docs/PLANO.md): o tráfego do injetor para quando ele
recebe "off" e volta com "on", e NENHUM quadro de controle chega à captura do
barramento. Este script lê a captura do barramento, os MACs dos injetores
(out-*/injector.mac) e a linha do tempo da rodada (com T0_EPOCH e as linhas
"ctrl ..."), e imprime:

  * quantos quadros de controle (porta UDP de controle) há na captura do barramento;
  * por janela de N segundos, quantos quadros cada injetor pôs no fio, com os
    comandos de controle marcados na janela em que aconteceram.

Uso:
    python3 tools/attack_activity.py --pcap out-dos/scn.pcap --macs out-dos/injector.mac \\
            --timeline out-dos/linha-do-tempo.txt [--ctrl-port 7000] [--bin 5]
"""
import argparse
import struct
from collections import Counter, defaultdict


def le_pcap(caminho):
    with open(caminho, "rb") as fh:
        cab = fh.read(24)
        end = "<" if struct.unpack("<I", cab[:4])[0] in (0xA1B2C3D4, 0xA1B23C4D) else ">"
        while True:
            reg = fh.read(16)
            if len(reg) < 16:
                return
            s, u, incl, _o = struct.unpack(end + "IIII", reg)
            yield s + u * 1e-6, fh.read(incl)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pcap", required=True)
    ap.add_argument("--macs", required=True)
    ap.add_argument("--timeline", required=True)
    ap.add_argument("--ctrl-port", type=int, default=7000)
    ap.add_argument("--bin", type=float, default=5.0)
    a = ap.parse_args()

    papeis = {}
    with open(a.macs, encoding="utf-8") as fh:
        for linha in fh:
            p = linha.split()
            if len(p) == 2:
                papeis[p[0].lower()] = p[1]
    t0 = None
    marcas = defaultdict(list)
    with open(a.timeline, encoding="utf-8") as fh:
        for linha in fh:
            if linha.startswith("T0_EPOCH="):
                t0 = float(linha.split("=", 1)[1])
            elif linha.strip():
                rel = float(linha.split()[0].replace(",", "."))
                texto = linha.split(None, 1)[1].strip()
                if texto.startswith("ctrl") or "injetor" in texto:
                    marcas[int(rel // a.bin)].append(texto)
    if t0 is None:
        raise SystemExit("ERRO: a linha do tempo não tem T0_EPOCH")

    por_janela = defaultdict(Counter)
    controle = 0
    for ts, raw in le_pcap(a.pcap):
        if len(raw) >= 38 and raw[12:14] == b"\x08\x00" and raw[23] == 17:
            ihl = (raw[14] & 0x0F) * 4
            sport, dport = struct.unpack(">HH", raw[14 + ihl:14 + ihl + 4])
            if a.ctrl_port in (sport, dport):
                controle += 1
        papel = papeis.get(raw[6:12].hex(":"))
        if papel:
            por_janela[int((ts - t0) // a.bin)][papel] += 1

    print("quadros de controle (UDP %d) na captura do barramento: %d" % (a.ctrl_port, controle))
    nomes = sorted(set(papeis.values()))
    print("  %-13s %s" % ("janela (s)", "  ".join("%14s" % n for n in nomes)))
    ultima = max(list(por_janela) + list(marcas) + [0])
    for w in range(0, ultima + 1):
        linha = "  %5.0f-%-6.0f %s" % (w * a.bin, (w + 1) * a.bin,
                                       "  ".join("%14d" % por_janela[w][n] for n in nomes))
        if marcas.get(w):
            linha += "   <- " + "; ".join(marcas[w])
        print(linha)


if __name__ == "__main__":
    main()
