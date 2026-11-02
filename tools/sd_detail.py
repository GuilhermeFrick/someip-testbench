#!/usr/bin/env python3
"""Detalhe das entradas de oferta do SD por (MAC de origem, IP de origem, TTL, endpoint).

Responde a uma pergunta que a regra de rótulo por IP não enxerga: uma oferta que
sai com o IP de uma ECU legítima foi mesmo enviada por ela? O MAC de origem
denuncia quem colocou o quadro no fio. Também separa OfferService (TTL > 0) de
StopOfferService (TTL 0) e mostra para qual endpoint cada uma aponta.

Uso:
    python3 tools/sd_detail.py <pcap> [--service 0x1001] [--max-frames N]
"""
import argparse
import struct
from collections import Counter


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


def entradas(raw):
    if len(raw) < 42 or raw[12:14] != b"\x08\x00" or raw[23] != 17:
        return
    mac = raw[6:12].hex(":")
    ihl = (raw[14] & 0x0F) * 4
    src = ".".join(str(b) for b in raw[26:30])
    dst = ".".join(str(b) for b in raw[30:34])
    c = raw[14 + ihl + 8:]
    if len(c) < 28 or c[0:2] != b"\xff\xff":
        return
    pl = c[16:]
    n_ent = struct.unpack(">I", pl[4:8])[0]
    ent = pl[8:8 + n_ent]
    base = 8 + n_ent
    ops = []
    if base + 4 <= len(pl):
        n_op = struct.unpack(">I", pl[base:base + 4])[0]
        j = base + 4
        while j + 3 <= min(base + 4 + n_op, len(pl)):
            olen, otyp = struct.unpack(">HB", pl[j:j + 3])
            if otyp == 0x04 and j + 12 <= len(pl):
                ops.append("%s:%d/%s" % (".".join(str(b) for b in pl[j + 4:j + 8]),
                                         struct.unpack(">H", pl[j + 10:j + 12])[0],
                                         {6: "tcp", 17: "udp"}.get(pl[j + 9], "?")))
            j += 3 + olen
    for k in range(0, len(ent) - 15, 16):
        typ, i1, nops = ent[k], ent[k + 1], ent[k + 3] >> 4
        svc = struct.unpack(">H", ent[k + 4:k + 6])[0]
        ttl = int.from_bytes(ent[k + 9:k + 12], "big")
        nome = {0x00: "Find", 0x01: "Offer" if ttl else "StopOffer",
                0x06: "Subscribe" if ttl else "StopSubscribe", 0x07: "SubAck" if ttl else "SubNack"}.get(typ, hex(typ))
        yield mac, src, dst, nome, svc, " ".join(sorted(ops[i1:i1 + nops]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--service", default="0x1001")
    ap.add_argument("--max-frames", type=int, default=3000000)
    a = ap.parse_args()
    alvo = int(a.service, 16)

    macs = {}
    cont = Counter()
    for idx, raw in enumerate(le_pcap(a.pcap)):
        if idx >= a.max_frames:
            break
        if len(raw) >= 34 and raw[12:14] == b"\x08\x00":
            ip = ".".join(str(b) for b in raw[26:30])
            macs.setdefault(raw[6:12].hex(":"), set()).add(ip)
        for mac, src, dst, nome, svc, eps in entradas(raw) or []:
            if svc == alvo and nome in ("Offer", "StopOffer"):
                cont[(mac, src, dst, nome, eps)] += 1

    print(a.pcap)
    print("  MAC de origem -> IPs que ele usou:")
    for mac, ips in sorted(macs.items(), key=lambda kv: sorted(kv[1])):
        print("    %s  %s" % (mac, ", ".join(sorted(ips))))
    print("  ofertas do serviço %s (MAC, IP origem, destino, tipo, endpoints):" % a.service)
    for (mac, src, dst, nome, eps), n in cont.most_common(20):
        print("    %8d  %s  %-12s -> %-16s %-9s %s" % (n, mac, src, dst, nome, eps or "-"))


if __name__ == "__main__":
    main()
