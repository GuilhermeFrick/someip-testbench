#!/usr/bin/env python3
"""Manda um comando de controle a um injetor, pela rede ctrl (fase D de docs/PLANO.md).

O injetor escuta comandos de uma linha em UDP no endereço da rede ctrl, que nunca é
capturada (src/nodes/CtrlChannel.hpp): on, off, status, set period_ms N,
set so_period_ms N, set so_target A.B.C.D. Este script manda o comando, espera a
resposta e imprime as duas com o horário epoch, para a linha do tempo da rodada.

Uso:
    python3 tools/inj_ctl.py --to 172.29.0.13:7000 off
    python3 tools/inj_ctl.py --to 172.29.0.13:7000 set period_ms 50
"""
import argparse
import socket
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--to", required=True, help="endereço:porta do injetor na rede ctrl")
    ap.add_argument("--timeout", type=float, default=2.0)
    ap.add_argument("comando", nargs="+")
    a = ap.parse_args()

    host, _, porta = a.to.rpartition(":")
    linha = " ".join(a.comando)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(a.timeout)
    t = time.time()
    s.sendto(linha.encode(), (host, int(porta)))
    try:
        resposta, _ = s.recvfrom(512)
        print("%.6f %s \"%s\" -> %s" % (t, a.to, linha, resposta.decode().strip()))
    except socket.timeout:
        print("%.6f %s \"%s\" -> SEM RESPOSTA em %.1f s" % (t, a.to, linha, a.timeout))
        sys.exit(1)


if __name__ == "__main__":
    main()
