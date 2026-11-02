#!/usr/bin/env python3
"""O que o atacante publica: cópia da VDE, conteúdo próprio ou constante?

Para cada PCAP de ataque, separa as notificações do serviço alvo (0x1001, da VDE)
por quem as enviou -- a VDE verdadeira ou o atacante, identificado pelo MAC, que
não pode ser forjado como o IP -- e compara os payloads, evento a evento:

  * quantos payloads distintos cada lado envia;
  * que fração dos payloads do atacante aparece, idêntica, entre os da VDE;
  * um exemplo de cada lado.

Responde ao risco 2 da fase A0 (docs/PLANO.md): o atacante vivo precisa publicar
o mesmo tipo de conteúdo que o do dataset.

Uso:
    python3 tools/attacker_payload.py <pcap> --vde-mac 02:42:ac:12:00:0a [--service 0x1001]
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
            _s, _u, incl, _o = struct.unpack(end + "IIII", reg)
            yield fh.read(incl)


def notificacoes(raw, alvo):
    if len(raw) < 34 or raw[12:14] != b"\x08\x00":
        return
    mac = raw[6:12].hex(":")
    ihl = (raw[14] & 0x0F) * 4
    l4 = 14 + ihl
    if raw[23] == 17:
        c, tr = raw[l4 + 8:], "udp"
    elif raw[23] == 6:
        c, tr = raw[l4 + ((raw[l4 + 12] >> 4) * 4):], "tcp"
    else:
        return
    i = 0
    while i + 16 <= len(c):
        svc, mth, ln = struct.unpack(">HHI", c[i:i + 8])
        fim = i + 8 + ln
        if ln < 8 or fim > len(c):
            return
        if svc == alvo and c[i + 14] == 0x02:
            yield mac, tr, mth, bytes(c[i + 16:fim])
        i = fim


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--vde-mac", required=True)
    ap.add_argument("--service", default="0x1001")
    ap.add_argument("--ref-service", default=None,
                    help="serviço da VDE com que comparar (padrão: o mesmo); 0x1001 para o relay 0x100b")
    ap.add_argument("--max-frames", type=int, default=3000000)
    a = ap.parse_args()
    alvo = int(a.service, 16)
    ref = int(a.ref_service, 16) if a.ref_service else alvo

    pl = defaultdict(Counter)          # (lado, mac, evento) -> payloads
    trans = Counter()
    for idx, raw in enumerate(le_pcap(a.pcap)):
        if idx >= a.max_frames:
            break
        for mac, tr, mth, p in list(notificacoes(raw, alvo) or []) + (
                [x for x in notificacoes(raw, ref) or [] if x[0] == a.vde_mac] if ref != alvo else []):
            lado = "VDE" if mac == a.vde_mac else "atacante"
            pl[(lado, mac, mth)][p] += 1
            trans[(lado, mac, tr)] += 1

    print(a.pcap)
    for (lado, mac, tr), n in sorted(trans.items()):
        print("  %-8s %s %s: %d notificações" % (lado, mac, tr, n))
    vde = defaultdict(set)
    for (lado, _m, mth), c in pl.items():
        if lado == "VDE":
            vde[mth] |= set(c)
    for (lado, mac, mth), c in sorted(pl.items()):
        total = sum(c.values())
        linha = "  %-8s %s evento 0x%04x: %7d msgs, %6d payloads distintos" % (lado, mac, mth, total, len(c))
        if lado == "atacante":
            iguais = sum(n for p, n in c.items() if p in vde.get(mth, ()))
            linha += ", %5.1f%% idênticos a algum da VDE" % (100.0 * iguais / total)
        print(linha)
        mais, _ = c.most_common(1)[0]
        print("           mais frequente (%d B): %s" % (len(mais), mais.hex()))


if __name__ == "__main__":
    main()
