#!/usr/bin/env python3
"""Detalhe dos StopOfferService que o atacante manda em nome do provedor legítimo.

Nos dois MITM do dataset do Kim, o atacante manda à ADAS uma entrada de oferta
com TTL 0 (StopOffer) para o serviço da VDE, com os endpoints da VDE; no MITM
multi, com o IP de origem da VDE forjado. Esse quadro não sai da API do vsomeip,
então o injetor vivo precisa montá-lo à mão, e montar igual exige saber:

  * IP e porta de origem e destino, e o MAC que de fato o enviou;
  * o cabeçalho SD: flags (reboot, unicast) e a sessão do SOME/IP;
  * entradas e opções do quadro;
  * QUANDO ele sai: intervalo entre envios e atraso em relação à oferta
    multicast mais recente da VDE verdadeira (identificada pelo MAC dela).

Uso:
    python3 tools/stopoffer_detail.py <pcap> --vde-mac 02:42:ac:12:00:0a [--service 0x1001]
"""
import argparse
import statistics
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
            s, u, incl, _o = struct.unpack(end + "IIII", reg)
            yield s + u * 1e-6, fh.read(incl)


def sd(raw):
    """(mac, src, sport, dst, dport, sessão, flags, entradas, opções) de um quadro UDP de SD."""
    if len(raw) < 42 or raw[12:14] != b"\x08\x00" or raw[23] != 17:
        return None
    ihl = (raw[14] & 0x0F) * 4
    l4 = 14 + ihl
    sport, dport = struct.unpack(">HH", raw[l4:l4 + 4])
    c = raw[l4 + 8:]
    if len(c) < 28 or c[0:2] != b"\xff\xff":
        return None
    sessao = struct.unpack(">H", c[10:12])[0]
    pl = c[16:]
    flags = pl[0]
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
            else:
                ops.append("opt0x%02x" % otyp)
            j += 3 + olen
    entradas = []
    for k in range(0, len(ent) - 15, 16):
        typ, i1, i2, nops = ent[k], ent[k + 1], ent[k + 2], ent[k + 3]
        svc, inst = struct.unpack(">HH", ent[k + 4:k + 8])
        maj = ent[k + 8]
        ttl = int.from_bytes(ent[k + 9:k + 12], "big")
        mino = struct.unpack(">I", ent[k + 12:k + 16])[0]
        entradas.append((typ, svc, inst, maj, ttl, mino, i1, i2, nops))
    return (raw[6:12].hex(":"), ".".join(str(b) for b in raw[26:30]), sport,
            ".".join(str(b) for b in raw[30:34]), dport, sessao, flags, entradas, ops)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--vde-mac", required=True)
    ap.add_argument("--service", default="0x1001")
    ap.add_argument("--max-frames", type=int, default=3000000)
    a = ap.parse_args()
    alvo = int(a.service, 16)

    ultima_oferta_vde = None
    envios = []
    atrasos = []
    formas = Counter()
    exemplo = None
    sessoes = []
    for idx, (ts, raw) in enumerate(le_pcap(a.pcap)):
        if idx >= a.max_frames:
            break
        q = sd(raw)
        if q is None:
            continue
        mac, src, sport, dst, dport, sessao, flags, entradas, ops = q
        for (typ, svc, inst, maj, ttl, mino, i1, i2, nops) in entradas:
            if typ != 0x01 or svc != alvo:
                continue
            if ttl > 0 and mac == a.vde_mac and dst.startswith("224."):
                ultima_oferta_vde = ts
            if ttl == 0 and mac != a.vde_mac:
                envios.append(ts)
                sessoes.append(sessao)
                if ultima_oferta_vde is not None:
                    atrasos.append(ts - ultima_oferta_vde)
                formas[("MAC %s" % mac, "%s:%d -> %s:%d" % (src, sport, dst, dport), "flags 0x%02x" % flags,
                        "%d entrada(s)" % len(entradas), "inst 0x%04x maj %d min %d" % (inst, maj, mino),
                        " ".join(ops))] += 1
                if exemplo is None:
                    exemplo = raw.hex()

    print(a.pcap)
    print("  StopOffer de 0x%04x vindos de MAC diferente do da VDE: %d" % (alvo, len(envios)))
    for forma, n in formas.most_common(8):
        print("    %7d  %s" % (n, " | ".join(forma)))
    if len(envios) > 1:
        gaps = [b - x for x, b in zip(envios, envios[1:])]
        print("  intervalo entre envios: mediana %.4f s, mínimo %.4f s, máximo %.1f s" % (
            statistics.median(gaps), min(gaps), max(gaps)))
    if atrasos:
        print("  atraso desde a última oferta multicast da VDE: mediana %.4f s, p10 %.4f s, p90 %.4f s" % (
            statistics.median(atrasos), sorted(atrasos)[len(atrasos) // 10], sorted(atrasos)[9 * len(atrasos) // 10]))
    if sessoes:
        print("  sessões: primeira %d, última %d, distintas %d" % (sessoes[0], sessoes[-1], len(set(sessoes))))
    if exemplo:
        print("  quadro de exemplo (hex): %s" % exemplo)


if __name__ == "__main__":
    main()
