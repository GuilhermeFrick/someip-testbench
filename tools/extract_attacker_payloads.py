#!/usr/bin/env python3
"""Extrai, em ordem, o que um ATACANTE do dataset publicou, para o injetor reproduzir.

No MITM single do dataset do Kim, o atacante .13 publica à ADAS ~93
notificações por segundo do evento 0x0001 da VDE com um sinal de velocidade
falso e plausível (média 14,7 m/s com o carro parado; tools/attacker_payload.py).
Reproduzir o ataque é publicar ESSE conteúdo. Não é copiar o barramento
benigno: o conteúdo benigno continua sendo o da bancada (e, depois, o do CARLA);
o que se copia é o dado falso que define o ataque.

O atacante é identificado pelo MAC de origem, que não pode ser forjado como o IP.

Saída no formato SPAY v1 de tools/extract_payloads.py (lido por
src/nodes/PayloadBank.hpp), com um fluxo:
    magic "SPAY", versao u32 = 1, nFluxos u32 = 1,
    servico u16, evento u16, tamanho u32, nAmostras u32, amostras.

Uso:
    python3 tools/extract_attacker_payloads.py --pcap mitm_single_attacker.pcap \\
            --mac 02:42:ac:12:00:0d --service 0x1001 --event 0x0001 \\
            --out build/cfg/attacker-mitm_single.bin
"""
import argparse
import struct
import sys
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pcap", required=True)
    ap.add_argument("--mac", required=True, help="MAC de origem do atacante")
    ap.add_argument("--service", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--event", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--max", type=int, default=200000, help="amostras no máximo")
    a = ap.parse_args()

    amostras = []
    for raw in le_pcap(a.pcap):
        if len(raw) < 34 or raw[12:14] != b"\x08\x00" or raw[6:12].hex(":") != a.mac.lower():
            continue
        ihl = (raw[14] & 0x0F) * 4
        l4 = 14 + ihl
        if raw[23] == 6:
            corpo = raw[l4 + ((raw[l4 + 12] >> 4) * 4):]
        elif raw[23] == 17:
            corpo = raw[l4 + 8:]
        else:
            continue
        i = 0
        while i + 16 <= len(corpo):
            svc, mth, ln = struct.unpack(">HHI", corpo[i:i + 8])
            fim = i + 8 + ln
            if ln < 8 or fim > len(corpo):
                break
            if svc == a.service and mth == a.event and corpo[i + 14] == 0x02:
                amostras.append(bytes(corpo[i + 16:fim]))
            i = fim
        if len(amostras) >= a.max:
            break

    if not amostras:
        sys.exit("ERRO: nenhuma notificação 0x%04x/0x%04x do MAC %s" % (a.service, a.event, a.mac))
    tamanhos = Counter(len(x) for x in amostras)
    if len(tamanhos) != 1:
        sys.exit("ERRO: tamanhos variáveis %s; o banco exige tamanho fixo" % dict(tamanhos))
    tam = len(amostras[0])
    with open(a.out, "wb") as fh:
        fh.write(b"SPAY")
        fh.write(struct.pack("<II", 1, 1))
        fh.write(struct.pack("<HHII", a.service, a.event, tam, len(amostras)))
        for x in amostras:
            fh.write(x)
    print("[attacker-payloads] %d amostras de %d B (%d distintas) do MAC %s -> %s" % (
        len(amostras), tam, len(set(amostras)), a.mac, a.out))


if __name__ == "__main__":
    main()
