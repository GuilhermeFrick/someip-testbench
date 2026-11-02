#!/usr/bin/env python3
"""Linha do tempo do início de um ataque: quem anunciou, assinou e conectou primeiro.

Imprime, com o tempo relativo ao primeiro quadro, a PRIMEIRA ocorrência de cada
evento relevante (oferta por origem, StopOffer, Subscribe/Ack por par, SYN por par
e porta) e, depois, contagens por janela de N segundos para os SYN, mostrando
quando as vítimas trocam de provedor. Serve para comparar a dinâmica do dataset
com a da bancada (platform/dup-offer).

Uso:
    python3 tools/attack_timeline.py <pcap> [--prefix 172.18.0.] [--window 30] [--max-frames N]
"""
import argparse
import struct
from collections import Counter, OrderedDict


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


def eventos(raw):
    if len(raw) < 34 or raw[12:14] != b"\x08\x00":
        return
    ihl = (raw[14] & 0x0F) * 4
    src = ".".join(str(b) for b in raw[26:30])
    dst = ".".join(str(b) for b in raw[30:34])
    l4 = 14 + ihl
    if raw[23] == 6:
        dport = struct.unpack(">H", raw[l4 + 2:l4 + 4])[0]
        if raw[l4 + 13] & 0x12 == 0x02:  # SYN sem ACK
            yield "SYN %s -> %s:%d" % (src, dst, dport)
        return
    if raw[23] != 17:
        return
    c = raw[l4 + 8:]
    if len(c) < 28 or c[0:2] != b"\xff\xff":
        return
    pl = c[16:]
    n_ent = struct.unpack(">I", pl[4:8])[0]
    ent = pl[8:8 + n_ent]
    for k in range(0, len(ent) - 15, 16):
        typ = ent[k]
        svc = struct.unpack(">H", ent[k + 4:k + 6])[0]
        ttl = int.from_bytes(ent[k + 9:k + 12], "big")
        nome = {0x00: "Find", 0x01: "Offer" if ttl else "StopOffer",
                0x06: "Subscribe" if ttl else "StopSubscribe", 0x07: "SubAck" if ttl else "SubNack"}.get(typ)
        if nome:
            alvo = "" if dst.startswith("224.") else " -> " + dst
            yield "%s 0x%04x %s%s" % (nome, svc, src, alvo)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--prefix", default="172.18.0.")
    ap.add_argument("--window", type=float, default=30.0)
    ap.add_argument("--max-frames", type=int, default=3000000)
    a = ap.parse_args()

    primeiro = OrderedDict()
    janelas = Counter()
    t0 = None
    for idx, (ts, raw) in enumerate(le_pcap(a.pcap)):
        if idx >= a.max_frames:
            break
        t0 = ts if t0 is None else t0
        for e in eventos(raw) or []:
            if e not in primeiro:
                primeiro[e] = ts - t0
            if e.startswith("SYN"):
                janelas[(int((ts - t0) // a.window), e.split(" -> ")[1])] += 1

    print(a.pcap)
    print("  primeira ocorrência (s):")
    for e, t in primeiro.items():
        print("    %9.3f  %s" % (t, e.replace(a.prefix, ".")))
    print("  SYN por janela de %.0f s, por destino:" % a.window)
    for (w, dst), n in sorted(janelas.items()):
        print("    %6.0f-%-6.0f %-22s %d" % (w * a.window, (w + 1) * a.window, dst.replace(a.prefix, "."), n))


if __name__ == "__main__":
    main()
