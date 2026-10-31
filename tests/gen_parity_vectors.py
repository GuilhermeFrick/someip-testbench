#!/usr/bin/env python3
"""Gera os vetores de referência para o teste de paridade do extrator.

Importa o `FeatureStreamer` REAL de `src/extract_ext.py` do repositório de
reprodução, monta uma sequência sintética de pacotes que exercita de propósito
os casos difíceis, e escreve dois arquivos:

    parity_bytemodels.bin   as tabelas (L, 256) em float64, no formato que o
                            ids::ByteModels::Create lê
    parity_vectors.txt      um registro por pacote: os campos do pacote e as 16
                            features que o Python produziu

O lado C++ (tests/test_parity_streamer.cpp) consome os dois e compara. Não é
"reimplementei e deu parecido": a referência é o código que treinou o modelo.

Casos exercitados, escolhidos porque cada um já quebrou alguma implementação:

  1. primeiro pacote de um fluxo              (tudo que é diferença vale zero)
  2. payload SOME/IP VAZIO, pacote de SD      (pl_someip vazio, pl_sd cheio)
  3. payload repetido no mesmo fluxo          (repeat_rate acima de zero)
  4. janela de repetição transbordando        (maxlen=5 descarta o mais antigo)
  5. payload vazio NO MESMO FLUXO dos        (entra na de repetição, NÃO entra
     não-vazios                                na de diversidade)
  6. dois fluxos da MESMA origem              (diversidade é por origem, não por
                                               fluxo)
  7. payload que cresce e que encurta         (Hamming soma a diferença de
                                               comprimento)
  8. payload mais longo que L                 (truncamento em 64/80 bytes)
  9. duas origens distintas                   (janelas independentes)

Uso, a partir da raiz do someip-testbench:
    python tests/gen_parity_vectors.py --repro ../someip-ids-multiclass-contentext \\
                                       --out build/parity
"""
from __future__ import annotations

import argparse
import os
import struct
import sys


def build_models(bytemodel_mod, extract_mod):
    """Treina os três modelos de bytes em payloads sintéticos, de forma determinística.

    Não usa o PCAP benigno: o objetivo é ter tabelas iguais nos dois lados, não
    tabelas realistas. O conteúdo do treino é fixo, então o resultado é
    reproduzível em qualquer máquina.
    """
    L = extract_mod.L
    models = {}
    for nome, comprimento in L.items():
        m = bytemodel_mod.ByteModel(comprimento, alpha=1.0)
        # Payloads de treino deterministicos, cobrindo valores de byte variados em
        # posicoes variadas, para a tabela nao ficar uniforme (uma tabela uniforme
        # esconderia erro de indexacao posicional).
        for k in range(40):
            carga = bytes(((i * 7 + k * 31) % 256) for i in range(comprimento))
            m.update(carga)
        for k in range(10):
            carga = bytes(((i * 3 + k) % 17) for i in range(comprimento // 2))
            m.update(carga)
        models[nome] = m.finalize()
    return models


def dump_models(models, extract_mod, path):
    """Escreve as tabelas no formato que o ids::ByteModels::Create espera."""
    ordem = ["someip", "sd", "l4"]
    with open(path, "wb") as fh:
        for nome in ordem:
            m = models[nome]
            posicoes = extract_mod.L[nome]
            assert m.logP.shape == (posicoes, 256), m.logP.shape
            fh.write(struct.pack("<I", posicoes))
            for i in range(posicoes):
                fh.write(struct.pack("<256d", *m.logP[i].tolist()))
    return os.path.getsize(path)


def ip_u32(dotted: str) -> int:
    """`172.30.0.11` -> 0xAC1E000B, que e como o lado C++ chaveia."""
    a, b, c, d = (int(x) for x in dotted.split("."))
    return (a << 24) | (b << 16) | (c << 8) | d


def build_packets(someip_mod):
    """Monta a sequência sintética. Cada entrada vira um `someip.Pkt`."""
    SD = 30490
    pacotes = []

    def pkt(ts, src, dst, sport, dport, proto, someip_body, sd_body):
        """Monta um Pkt coerente: pl_l4 = 16 bytes de cabecalho + corpo."""
        corpo = someip_body if someip_body else sd_body
        cabecalho = bytes(range(16))
        pl_l4 = cabecalho + corpo
        p = someip_mod.Pkt(ts=ts, src=src, dst=dst, proto=proto, sport=sport, dport=dport,
                           ip_len=20 + 8 + len(pl_l4))
        p.pl_l4 = pl_l4
        p.pl_someip = someip_body
        p.pl_sd = sd_body
        return p

    A = "172.30.0.11"
    B = "172.30.0.12"
    C = "172.30.0.13"

    P1 = bytes([0xAA] * 8)
    P2 = bytes([0xBB] * 8)
    P3 = bytes([0xAA] * 12)          # cresce
    P4 = bytes([0xAA] * 4)           # encurta
    LONGO = bytes(((i * 13) % 256) for i in range(200))   # acima de L

    # 1. primeiro pacote do fluxo A->B
    pacotes.append(pkt(1.000, A, B, 40000, 30509, 17, P1, b""))
    # 3. payload repetido: repeat_rate deve subir
    pacotes.append(pkt(1.100, A, B, 40000, 30509, 17, P1, b""))
    pacotes.append(pkt(1.250, A, B, 40000, 30509, 17, P1, b""))
    # payload diferente
    pacotes.append(pkt(1.400, A, B, 40000, 30509, 17, P2, b""))
    # 7. cresce e encurta: Hamming soma a diferenca de comprimento
    pacotes.append(pkt(1.550, A, B, 40000, 30509, 17, P3, b""))
    pacotes.append(pkt(1.700, A, B, 40000, 30509, 17, P4, b""))
    # 2. pacote de SD nas portas de SD: someip VAZIO, sd cheio, OUTRO fluxo
    pacotes.append(pkt(1.800, A, B, SD, SD, 17, b"", bytes([0xCC] * 20)))
    # 5. payload someip VAZIO NO MESMO FLUXO dos nao-vazios. Este e o caso que
    # exercita a assimetria entre as duas janelas, e a primeira versao deste
    # gerador NAO o cobria: o unico pacote de someip vazio era o de SD acima, que
    # tem portas de SD e portanto cai noutro fluxo. Sem isto, trocar a janela de
    # repeticao para rejeitar vazio passava no teste.
    # Fisicamente acontece quando o corpo SOME/IP tem tamanho zero (pl_l4 com so
    # os 16 bytes de cabecalho), que e o caso de um evento sem carga.
    pacotes.append(pkt(1.850, A, B, 40000, 30509, 17, b"", b""))
    # e agora nao-vazios DEPOIS do vazio, para o vazio estar dentro da janela de
    # 5 quando o repeat_rate for calculado
    pacotes.append(pkt(1.900, A, B, 40000, 30509, 17, P1, b""))
    pacotes.append(pkt(1.950, A, B, 40000, 30509, 17, P1, b""))
    # 4. enche a janela de repeticao (maxlen=5) e transborda
    for i in range(6):
        pacotes.append(pkt(2.000 + 0.05 * i, A, B, 40000, 30509, 17, P1, b""))
    # 8. payload acima de L
    pacotes.append(pkt(2.400, A, B, 40000, 30509, 17, LONGO, b""))
    # 6. SEGUNDO fluxo da MESMA origem: diversidade e por origem
    pacotes.append(pkt(2.500, A, C, 40001, 30509, 17, P2, b""))
    pacotes.append(pkt(2.600, A, C, 40001, 30509, 17, P1, b""))
    # 9. outra origem: janelas independentes
    pacotes.append(pkt(2.700, B, A, 40002, 30509, 17, P1, b""))
    pacotes.append(pkt(2.800, B, A, 40002, 30509, 17, P1, b""))
    # volta ao primeiro fluxo, para conferir que o estado dele sobreviveu
    pacotes.append(pkt(3.000, A, B, 40000, 30509, 17, P2, b""))
    # TCP, so para o proto entrar na chave de fluxo
    pacotes.append(pkt(3.100, A, B, 40000, 30509, 6, P1, b""))
    return pacotes


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repro", required=True, help="raiz do repo someip-ids-multiclass-contentext")
    ap.add_argument("--out", default="build/parity")
    a = ap.parse_args()

    sys.path.insert(0, os.path.join(a.repro, "src"))
    import bytemodel as bytemodel_mod   # noqa: E402
    import extract as extract_mod       # noqa: E402
    import someip as someip_mod         # noqa: E402
    import extract_ext as ext_mod       # noqa: E402

    os.makedirs(a.out, exist_ok=True)

    models = build_models(bytemodel_mod, extract_mod)
    bin_path = os.path.join(a.out, "parity_bytemodels.bin")
    tamanho = dump_models(models, extract_mod, bin_path)
    print("[1/3] tabelas de bytes -> %s (%d bytes, L=%s)" % (bin_path, tamanho, extract_mod.L))

    pacotes = build_packets(someip_mod)
    print("[2/3] %d pacotes sinteticos" % len(pacotes))

    # O streamer REAL do repo de reproducao. E isto que torna o teste uma
    # paridade, e nao duas implementacoes minhas concordando entre si.
    streamer = ext_mod.FeatureStreamer()

    txt_path = os.path.join(a.out, "parity_vectors.txt")
    with open(txt_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# gerado por tests/gen_parity_vectors.py; referencia = src/extract_ext.py\n")
        fh.write("# campos: ts src dst sport dport proto ip_len pl_l4 pl_someip pl_sd | 16 features\n")
        fh.write("# enderecos como uint32; payloads em hex; vazio = \"-\"\n")
        for p in pacotes:
            f = streamer.step(p, models)
            assert len(f) == 16, len(f)
            campos = [
                "%.9f" % p.ts,
                "%u" % ip_u32(p.src),
                "%u" % ip_u32(p.dst),
                "%u" % p.sport,
                "%u" % p.dport,
                "%u" % p.proto,
                "%u" % p.ip_len,
                p.pl_l4.hex() if p.pl_l4 else "-",
                p.pl_someip.hex() if p.pl_someip else "-",
                p.pl_sd.hex() if p.pl_sd else "-",
            ]
            vals = ["%.17g" % float(v) for v in f]
            fh.write(" ".join(campos) + " | " + " ".join(vals) + "\n")
    print("[3/3] vetores de referencia -> %s" % txt_path)
    print()
    print("Agora, no someip-testbench:")
    print("  g++ -std=c++14 -I. -o /tmp/tp tests/test_parity_streamer.cpp \\")
    print("      src/ids_sensor/ByteModel.cpp src/ids_sensor/FeatureStreamer.cpp \\")
    print("      src/ids_sensor/IdsSensorErrorDomain.cpp")
    print("  /tmp/tp %s %s" % (bin_path, txt_path))


if __name__ == "__main__":
    main()
