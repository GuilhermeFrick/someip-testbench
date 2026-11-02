#!/usr/bin/env python3
"""Tenta recuperar a SEMÂNTICA de cada evento do barramento de referência.

O PCAP carrega IP e IDs de serviço/evento, nunca o nome da ECU. O artigo do Kim
nomeia os nove nós (GPS, IMU, VDE como provedores; ADAS, CLU, NAV, PT, STE, TEL
como consumidores), mas não diz qual IP é qual. Este script lê os payloads de
cada (origem, serviço, evento) e os interpreta como sequência de números em
vários formatos, para que a assinatura física diga quem é quem: latitude e
longitude num GPS, gravidade (~9,8) num acelerômetro, velocidade numa dinâmica.

Não decide nada sozinho: imprime estatística por campo, e a leitura é humana.

Uso:
    python3 tools/decode_payload_semantics.py <pcap> [--max-frames N]
"""
import argparse
import math
import struct
import sys
from collections import defaultdict

SD_SERVICE = 0xFFFF


def mensagens(raw):
    """Extrai (src, dst, proto, bytes-someip) de um quadro Ethernet/IPv4."""
    if len(raw) < 34 or raw[12:14] != b"\x08\x00":
        return
    ihl = (raw[14] & 0x0F) * 4
    proto = raw[23]
    src = ".".join(str(b) for b in raw[26:30])
    dst = ".".join(str(b) for b in raw[30:34])
    l4 = 14 + ihl
    if proto == 17:
        corpo = raw[l4 + 8:]
    elif proto == 6:
        off = (raw[l4 + 12] >> 4) * 4
        corpo = raw[l4 + off:]
    else:
        return
    # TCP pode carregar várias mensagens SOME/IP num segmento: percorre pelo length.
    i = 0
    while i + 16 <= len(corpo):
        svc, mth, ln = struct.unpack(">HHI", corpo[i:i + 8])
        fim = i + 8 + ln
        if ln < 8 or fim > len(corpo):
            break
        yield src, dst, ("udp" if proto == 17 else "tcp"), svc, mth, corpo[i + 16:fim], corpo[i + 14]
        i = fim


def le_pcap(caminho):
    """Leitor mínimo de PCAP clássico (sem dependências): devolve os bytes de cada quadro."""
    with open(caminho, "rb") as fh:
        cab = fh.read(24)
        magic = struct.unpack("<I", cab[:4])[0]
        end = "<" if magic in (0xA1B2C3D4, 0xA1B23C4D) else ">"
        while True:
            reg = fh.read(16)
            if len(reg) < 16:
                return
            _s, _u, incl, _orig = struct.unpack(end + "IIII", reg)
            yield fh.read(incl)


def campos(payloads, fmt, largura):
    n = min(len(p) for p in payloads) // largura
    out = []
    for k in range(n):
        vals = []
        for p in payloads:
            v = struct.unpack(fmt, p[k * largura:(k + 1) * largura])[0]
            if isinstance(v, float) and (math.isnan(v) or math.isinf(v)):
                continue
            vals.append(v)
        if vals:
            out.append((k, min(vals), max(vals), sum(vals) / len(vals), len(set(vals))))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--max-frames", type=int, default=300000)
    a = ap.parse_args()

    amostras = defaultdict(list)
    destinos = defaultdict(set)
    for idx, raw in enumerate(le_pcap(a.pcap)):
        if idx >= a.max_frames:
            break
        for src, dst, tr, svc, mth, pl, mtype in mensagens(bytes(raw)) or []:
            if svc == SD_SERVICE or mtype != 0x02:  # só NOTIFICATION
                continue
            chave = (src, svc, mth, len(pl))
            if len(amostras[chave]) < 4000:
                amostras[chave].append(pl)
            destinos[chave].add(dst)

    for chave in sorted(amostras):
        src, svc, mth, n = chave
        ps = amostras[chave]
        print("=" * 78)
        print(f"origem {src}  serviço 0x{svc:04x}  evento 0x{mth:04x}  {n} bytes  "
              f"{len(ps)} amostras  destinos {sorted(destinos[chave])}")
        print(f"  exemplo hex: {ps[len(ps) // 2].hex()}")
        for nome, fmt, w in (("float64 LE", "<d", 8), ("float64 BE", ">d", 8),
                             ("float32 LE", "<f", 4), ("float32 BE", ">f", 4)):
            linhas = campos(ps, fmt, w)
            plaus = [l for l in linhas if all(abs(x) < 1e6 for x in l[1:4])]
            if len(plaus) != len(linhas) or not linhas:
                continue
            print(f"  {nome}:")
            for k, lo, hi, med, dist in linhas:
                print(f"    campo {k}: min {lo:12.5f}  max {hi:12.5f}  média {med:12.5f}  distintos {dist}")


if __name__ == "__main__":
    main()
