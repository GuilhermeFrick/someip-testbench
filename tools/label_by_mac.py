#!/usr/bin/env python3
"""Rótulo por quadro de uma captura da bancada, pelo MAC de origem dos injetores.

O dataset do Kim rotula por IP de origem (fora das 9 ECUs = ataque). Essa regra
falha no MITM multi: o injetor .15 manda à ADAS um StopOffer com o IP da VDE
forjado, e o quadro sai rotulado como normal (tools/sd_detail.py). Na bancada
quem coloca cada quadro no fio é conhecido pelo MAC, que o script da rodada fixa
para cada injetor e grava em out-*/injector.mac ("<mac> <papel>" por linha).

Saída no formato do experiments/eval-pcap/score_pcap.cpp: uma linha
"<índice> <classe>" por quadro, com as classes na ordem do detector
(0 normal, 1 dos, 2 fuzzy, 3 mitm_single, 4 mitm_multi).

Também conta quantos quadros a regra por IP do dataset rotularia diferente, e de
que tipo são, para que a diferença entre as duas regras seja um número e não uma
nota de rodapé.

Uso:
    python3 tools/label_by_mac.py --pcap out-mitm_multi/scn.pcap \\
            --macs out-mitm_multi/injector.mac --prefix 172.28.0. \\
            --out out-mitm_multi/scn.labels
"""
import argparse
import struct
from collections import Counter

CLASSE_DO_PAPEL = {"dos": 1, "fuzzy": 2, "mitm_single": 3, "mitm_relay": 4, "mitm_impostor": 4}
NOMES = {0: "normal", 1: "dos", 2: "fuzzy", 3: "mitm_single", 4: "mitm_multi"}


def le_pcap(caminho):
    with open(caminho, "rb") as fh:
        cab = fh.read(24)
        end = "<" if struct.unpack("<I", cab[:4])[0] in (0xA1B2C3D4, 0xA1B23C4D) else ">"
        while True:
            reg = fh.read(16)
            if len(reg) < 16:
                return
            _s, _u, incl, _o = struct.unpack(end + "IIII", reg)
            yield fh.read(incl)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pcap", required=True)
    ap.add_argument("--macs", required=True, help="arquivo '<mac> <papel>' por linha")
    ap.add_argument("--prefix", required=True, help="sub-rede, até o último ponto (172.28.0.)")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    macs = {}
    with open(a.macs, encoding="utf-8") as fh:
        for linha in fh:
            partes = linha.split()
            if len(partes) == 2:
                macs[partes[0].lower()] = CLASSE_DO_PAPEL[partes[1]]
    benignos = {a.prefix + str(i) for i in range(2, 11)}

    por_classe = Counter()
    divergem = Counter()
    n = 0
    with open(a.out, "w", encoding="utf-8", newline="\n") as out:
        for idx, raw in enumerate(le_pcap(a.pcap)):
            n += 1
            classe = macs.get(raw[6:12].hex(":"), 0) if len(raw) >= 14 else 0
            out.write("%d %d\n" % (idx, classe))
            por_classe[classe] += 1
            if len(raw) >= 34 and raw[12:14] == b"\x08\x00":
                src = ".".join(str(b) for b in raw[26:30])
                ataque_por_ip = (src not in benignos) and (src != a.prefix + "1")
                if ataque_por_ip != (classe != 0):
                    divergem[("ataque pelo MAC, normal pelo IP (origem %s)" % src) if classe else
                             ("normal pelo MAC, ataque pelo IP (origem %s)" % src)] += 1

    print("[label-by-mac] %d quadros -> %s" % (n, a.out))
    for c, k in sorted(por_classe.items()):
        print("  %-12s %8d (%.2f%%)" % (NOMES[c], k, 100.0 * k / n))
    if divergem:
        print("  onde a regra por IP do dataset rotularia diferente:")
        for desc, k in divergem.most_common():
            print("    %8d  %s" % (k, desc))
    else:
        print("  a regra por IP do dataset daria o mesmo rótulo em todos os quadros")


if __name__ == "__main__":
    main()
