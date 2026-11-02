#!/usr/bin/env python3
"""Cadência das ofertas de Service Discovery de cada origem, num PCAP.

Nos ataques do dataset o atacante anuncia um serviço que não é dele, e a
frequência desse anúncio é parte do mecanismo: com uma oferta a cada ciclo
normal do SD (2 s), as vítimas assinam no atacante mas mantêm a conexão TCP com
o provedor legítimo (platform/dup-offer). Este script mede, por origem e por
serviço oferecido, quantas ofertas houve, em quanto tempo e com que intervalo,
para que o atacante vivo anuncie no mesmo ritmo do dataset.

Uso:
    python3 tools/offer_cadence.py <pcap> [--max-frames N]
"""
import argparse
import statistics
import struct
from collections import defaultdict


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


def ofertas(raw):
    """(origem, serviço, ttl) de cada OfferService num quadro UDP de SD."""
    if len(raw) < 42 or raw[12:14] != b"\x08\x00" or raw[23] != 17:
        return
    ihl = (raw[14] & 0x0F) * 4
    src = ".".join(str(b) for b in raw[26:30])
    c = raw[14 + ihl + 8:]
    if len(c) < 28 or c[0:2] != b"\xff\xff":
        return
    pl = c[16:]
    n_ent = struct.unpack(">I", pl[4:8])[0]
    ent = pl[8:8 + n_ent]
    for k in range(0, len(ent) - 15, 16):
        if ent[k] == 0x01:
            svc = struct.unpack(">H", ent[k + 4:k + 6])[0]
            ttl = int.from_bytes(ent[k + 9:k + 12], "big")
            yield src, svc, ttl


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--max-frames", type=int, default=3000000)
    a = ap.parse_args()

    t = defaultdict(list)
    ttls = defaultdict(set)
    inicio = fim = None
    for idx, (ts, raw) in enumerate(le_pcap(a.pcap)):
        if idx >= a.max_frames:
            break
        inicio = ts if inicio is None else inicio
        fim = ts
        for src, svc, ttl in ofertas(raw) or []:
            t[(src, svc)].append(ts)
            ttls[(src, svc)].add(ttl)

    print("%s | %.1f s de captura" % (a.pcap, (fim or 0) - (inicio or 0)))
    print("  %-14s %-8s %8s %9s %12s %12s  %s" % ("origem", "serviço", "ofertas", "dur. (s)",
                                                 "ofertas/s", "interv. med.", "TTL"))
    linhas = sorted(t.items(), key=lambda kv: -len(kv[1]))
    for (src, svc), ts in linhas[:15]:
        dur = ts[-1] - ts[0]
        gaps = [b - x for x, b in zip(ts, ts[1:]) if b > x]
        med = statistics.median(gaps) if gaps else 0.0
        print("  %-14s 0x%04x %8d %9.1f %12.1f %10.4f s  %s" % (
            src, svc, len(ts), dur, len(ts) / dur if dur > 0 else 0.0, med,
            ",".join(str(x) for x in sorted(ttls[(src, svc)]))))
    if len(linhas) > 15:
        print("  ... %d outras combinações origem/serviço" % (len(linhas) - 15))


if __name__ == "__main__":
    main()
