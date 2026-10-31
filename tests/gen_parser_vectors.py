#!/usr/bin/env python3
"""Gera os vetores de referência do PARSER, a partir do `someip.parse` real.

Monta quadros Ethernet crus cobrindo os casos de borda, roda o parser do
repositório de reprodução em cada um, e grava o quadro mais o que ele devolveu.
O lado C++ (tests/test_parity_parser.cpp) compara campo a campo.

Por que montar os quadros à mão em vez de usar o PCAP: aqui eu escolho os casos,
inclusive os que o dataset pode não conter (VLAN, IPv6, opções de IP, padding
Ethernet). A paridade sobre o dataset inteiro é outro teste, e é o que pega o que
eu não soube imaginar.

Uso, a partir da raiz do someip-testbench:
    python tests/gen_parser_vectors.py --repro ../someip-ids-multiclass-contentext \\
                                       --out build/parity
"""
from __future__ import annotations

import argparse
import os
import struct
import sys


def eth(ethertype: int, payload: bytes) -> bytes:
    """Quadro Ethernet II: destino, origem e EtherType."""
    return b"\xaa" * 6 + b"\xbb" * 6 + struct.pack(">H", ethertype) + payload


def ipv4(proto: int, src: str, dst: str, payload: bytes, ihl_words: int = 5) -> bytes:
    """Cabeçalho IPv4 com IHL configurável, para exercitar opções de IP."""
    a = bytes(int(x) for x in src.split("."))
    b = bytes(int(x) for x in dst.split("."))
    opcoes = b"\x00" * ((ihl_words - 5) * 4)
    total = ihl_words * 4 + len(payload)
    cab = struct.pack(">BBHHHBBH", 0x40 | ihl_words, 0x00, total, 0x1234, 0x0000, 64, proto, 0x0000)
    return cab + a + b + opcoes + payload


def udp(sport: int, dport: int, payload: bytes) -> bytes:
    return struct.pack(">HHHH", sport, dport, 8 + len(payload), 0x0000) + payload


def tcp(sport: int, dport: int, payload: bytes, data_off_words: int = 5) -> bytes:
    extra = b"\x00" * ((data_off_words - 5) * 4)
    cab = struct.pack(">HHIIBBHHH", sport, dport, 1, 1, (data_off_words << 4), 0x18, 0xFFFF, 0x0000, 0x0000)
    return cab + extra + payload


def someip(service: int, method: int, body: bytes, msg_type: int = 0x02) -> bytes:
    """Cabeçalho SOME/IP de 16 bytes mais o corpo."""
    return (struct.pack(">HHI", service, method, 8 + len(body)) +
            struct.pack(">HHBBBB", 0x0001, 0x0002, 0x01, 0x01, msg_type, 0x00) + body)


def casos():
    """Os quadros, com um rótulo dizendo o que cada um exercita."""
    A, B = "172.30.0.11", "172.30.0.12"
    corpo = bytes([0xAA] * 8)
    out = []

    out.append(("udp someip normal",
                eth(0x0800, ipv4(17, A, B, udp(40000, 30509, someip(0x1234, 0x8777, corpo))))))
    out.append(("sd pela PORTA de origem",
                eth(0x0800, ipv4(17, A, B, udp(30490, 30490, someip(0xFFFF, 0x8100, bytes([0xCC] * 20)))))))
    out.append(("sd pelo SERVICE_ID 0xFFFF em porta normal",
                eth(0x0800, ipv4(17, A, B, udp(40000, 30509, someip(0xFFFF, 0x8100, bytes([0xDD] * 12)))))))
    out.append(("tcp someip",
                eth(0x0800, ipv4(6, A, B, tcp(40000, 30509, someip(0x1234, 0x8777, corpo))))))
    out.append(("tcp com data offset maior (opcoes)",
                eth(0x0800, ipv4(6, A, B, tcp(40000, 30509, someip(0x1234, 0x8777, corpo), data_off_words=8)))))
    out.append(("ip com OPCOES (ihl=7)",
                eth(0x0800, ipv4(17, A, B, udp(40000, 30509, someip(0x1234, 0x8777, corpo)), ihl_words=7))))
    out.append(("payload l4 MENOR que o cabecalho someip",
                eth(0x0800, ipv4(17, A, B, udp(40000, 30509, b"\x01\x02\x03")))))
    out.append(("payload l4 VAZIO",
                eth(0x0800, ipv4(17, A, B, udp(40000, 30509, b"")))))
    out.append(("corpo someip VAZIO (l4 com exatamente 16)",
                eth(0x0800, ipv4(17, A, B, udp(40000, 30509, someip(0x1234, 0x8777, b""))))))
    # Padding Ethernet: quadro abaixo de 60 bytes, completado com zeros. O
    # parser NAO corta em ip_len, entao o padding entra no payload.
    curto = eth(0x0800, ipv4(17, A, B, udp(40000, 30509, b"\x01\x02")))
    out.append(("quadro curto COM PADDING ethernet", curto + b"\x00" * (60 - len(curto))))
    out.append(("VLAN 802.1Q (deve ser descartado)",
                eth(0x8100, struct.pack(">HH", 0x0064, 0x0800) +
                    ipv4(17, A, B, udp(40000, 30509, someip(0x1234, 0x8777, corpo))))))
    out.append(("IPv6 (deve ser descartado)", eth(0x86DD, b"\x60" + b"\x00" * 60)))
    out.append(("ICMP, proto 1 (deve ser descartado)",
                eth(0x0800, ipv4(1, A, B, b"\x08\x00" + b"\x00" * 30))))
    out.append(("ARP (deve ser descartado)", eth(0x0806, b"\x00" * 40)))
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repro", required=True)
    ap.add_argument("--out", default="build/parity")
    a = ap.parse_args()

    sys.path.insert(0, os.path.join(a.repro, "src"))
    import someip as someip_mod   # noqa: E402

    os.makedirs(a.out, exist_ok=True)
    caminho = os.path.join(a.out, "parser_vectors.txt")

    n_pkt = 0
    n_none = 0
    n_crash = 0
    with open(caminho, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# gerado por tests/gen_parser_vectors.py; referencia = src/someip.py\n")
        fh.write("# formato: <rotulo> | <quadro em hex> | <resultado>\n")
        fh.write("# resultado: NONE, CRASH, ou:\n")
        fh.write("#   PKT src dst sport dport proto ip_len service method pl_l4 pl_someip pl_sd\n")
        fh.write("# (payloads em hex, vazio = \"-\"; service/method = -1 quando sem cabecalho someip)\n")
        for rotulo, quadro in casos():
            try:
                p = someip_mod.parse(quadro, 1.0)
            except Exception as e:                      # noqa: BLE001
                fh.write("%s | %s | CRASH %s\n" % (rotulo, quadro.hex(), type(e).__name__))
                n_crash += 1
                continue
            if p is None:
                fh.write("%s | %s | NONE\n" % (rotulo, quadro.hex()))
                n_none += 1
                continue
            def h(b):
                return b.hex() if b else "-"
            a4 = lambda s: int.from_bytes(bytes(int(x) for x in s.split(".")), "big")  # noqa: E731
            fh.write("%s | %s | PKT %u %u %u %u %u %u %d %d %s %s %s\n" % (
                rotulo, quadro.hex(), a4(p.src), a4(p.dst), p.sport, p.dport, p.proto,
                p.ip_len, p.service_id, p.method_id, h(p.pl_l4), h(p.pl_someip), h(p.pl_sd)))
            n_pkt += 1

    print("%d casos -> %s" % (n_pkt + n_none + n_crash, caminho))
    print("  pacote devolvido: %d" % n_pkt)
    print("  descartado (None): %d" % n_none)
    print("  excecao no parser: %d" % n_crash)


if __name__ == "__main__":
    main()
