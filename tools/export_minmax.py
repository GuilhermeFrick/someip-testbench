#!/usr/bin/env python3
"""Exporta os limites min-max do treino para o formato que o `ids::Normalizer` lê.

Formato, little-endian: `uint32` com o número de features, depois essa
quantidade de `double` de xmin, depois a mesma quantidade de xmax.

Duas fontes, e elas NÃO são equivalentes:

  --from-npy <minmax.npy>     A FONTE CORRETA. É o arquivo que o `pipeline.run`
                              do repositório de reprodução salva, com os limites
                              em float64 exatamente como o treino os calculou.
                              Use este sempre que ele existir.

  --from-records <dir>        Fonte de contorno, a partir dos registros `.f32`
                              da rodada de paridade. Os registros guardam as
                              features em float32, então os limites saem em
                              float32 arredondado.

Sobre a fonte de contorno, com precisão: o arredondamento float64 para float32 é
monótono, então o mínimo dos valores arredondados é exatamente o arredondamento
do mínimo. Os limites obtidos daqui são, portanto, `round_f32(xmin)` e
`round_f32(xmax)` dos limites verdadeiros — não uma aproximação de qualidade
desconhecida, e sim o arredondamento deles. Ainda assim, não é o artefato do
treino, e o `Normalizer` exige que os artefatos venham todos da mesma rodada.

Uso:
    python3 tools/export_minmax.py --from-npy results/minmax.npy --out build/cfg/minmax.bin
    python3 tools/export_minmax.py --from-records build/parity-dataset --out build/cfg/minmax.bin
"""
from __future__ import annotations

import argparse
import glob
import os
import struct
import sys

N_FEAT = 16
RECORD = struct.Struct("<I16f")


def de_npy(caminho: str):
    """Lê o minmax.npy do pipeline de treino: duas linhas, xmin e xmax."""
    import numpy as np

    m = np.load(caminho)
    if m.shape != (2, N_FEAT):
        raise SystemExit("minmax.npy com forma %s, esperado (2, %d)" % (m.shape, N_FEAT))
    return [float(v) for v in m[0]], [float(v) for v in m[1]], "treino (float64)"


def de_registros(diretorio: str):
    """Varre os registros .f32 da paridade e acumula o mínimo e o máximo por feature.

    A ordem dos arquivos não importa para mínimo e máximo, mas é a do manifesto
    para que o relatório saia comparável com o da rodada.
    """
    arquivos = sorted(glob.glob(os.path.join(diretorio, "*.f32")))
    if not arquivos:
        raise SystemExit("nenhum .f32 em %s. Rode antes a paridade do dataset." % diretorio)

    xmin = [float("inf")] * N_FEAT
    xmax = [float("-inf")] * N_FEAT
    total = 0
    for caminho in arquivos:
        with open(caminho, "rb") as fh:
            while True:
                # Lê em blocos: um registro por vez em 14 milhões de registros
                # seria uma chamada de sistema por registro.
                bloco = fh.read(RECORD.size * 8192)
                if not bloco:
                    break
                for (_, *vals) in RECORD.iter_unpack(bloco):
                    total += 1
                    for i in range(N_FEAT):
                        v = vals[i]
                        if v < xmin[i]:
                            xmin[i] = v
                        if v > xmax[i]:
                            xmax[i] = v
        print("      %-44s acumulado" % os.path.basename(caminho))
    print("      %d vetores lidos" % total)
    return xmin, xmax, "registros da paridade (float32 arredondado)"


def main() -> None:
    ap = argparse.ArgumentParser()
    fonte = ap.add_mutually_exclusive_group(required=True)
    fonte.add_argument("--from-npy", help="minmax.npy salvo pelo pipeline de treino")
    fonte.add_argument("--from-records", help="diretorio com os .f32 da paridade")
    ap.add_argument("--out", default="build/cfg/minmax.bin")
    ap.add_argument("--names", action="store_true", help="imprime os nomes das features no relatorio")
    a = ap.parse_args()

    if a.from_npy:
        xmin, xmax, origem = de_npy(a.from_npy)
    else:
        xmin, xmax, origem = de_registros(a.from_records)

    nomes = ["f%d" % i for i in range(N_FEAT)]
    if a.names:
        nomes = [
            "ip_time_interval", "someip_loglik", "sd_loglik", "l4_loglik",
            "someip_entropy", "sd_entropy", "l4_entropy",
            "someip_change", "sd_change", "l4_change", "ip_len_change", "l4_len_change",
            "repeat_rate", "someip_len", "l4_len", "src_payload_div",
        ]

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "wb") as fh:
        fh.write(struct.pack("<I", N_FEAT))
        fh.write(struct.pack("<%dd" % N_FEAT, *xmin))
        fh.write(struct.pack("<%dd" % N_FEAT, *xmax))

    print()
    print("fonte: %s" % origem)
    print("%-20s %18s %18s %s" % ("feature", "xmin", "xmax", ""))
    degeneradas = []
    for i in range(N_FEAT):
        aviso = ""
        if not (xmax[i] > xmin[i]):
            aviso = "<- DEGENERADA: divisor 1,0"
            degeneradas.append(nomes[i])
        print("%-20s %18.9g %18.9g %s" % (nomes[i], xmin[i], xmax[i], aviso))

    print()
    if degeneradas:
        # Nao e erro: a referencia divide por 1,0 nesse caso, e o Normalizer faz
        # igual. Mas e informacao que vale aparecer, porque uma feature constante
        # no treino nao carrega informacao nenhuma para as arvores.
        print("ATENCAO: %d feature(s) constante(s) no treino: %s" % (len(degeneradas), ", ".join(degeneradas)))
        print("         A referencia divide por 1,0 nesse caso, e o Normalizer reproduz isso.")
    else:
        print("nenhuma feature degenerada: todo divisor e xmax-xmin")
    print("%d bytes -> %s" % (os.path.getsize(a.out), a.out))


if __name__ == "__main__":
    main()
    sys.exit(0)
