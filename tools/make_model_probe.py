#!/usr/bin/env python3
"""Tira linhas de prova do DATASET REAL e as probabilidades que o XGBoost dá para elas.

Serve ao `tests/test_classifier.cpp`, que confere os dois motores em C++ contra
o `predict` do próprio XGBoost. Até agora as linhas vinham de um modelo de prova
treinado em ruído; aqui elas vêm do tráfego de verdade, o que importa por um
motivo concreto: **as regiões de decisão que o teste visita passam a ser as que
o sensor visita**. Com linhas aleatórias, um erro de travessia que só aparecesse
na vizinhança de um limiar real poderia não ser tocado nunca.

As linhas saem dos registros `.f32` da rodada de paridade, que guardam as
features BRUTAS, e são normalizadas aqui com os limites do treino, em float64 e
depois convertidas para float32 — a mesma ordem do `extract_all()` do pipeline.

A amostragem é determinística: um passo fixo sobre a concatenação dos arquivos,
na mesma ordem do manifesto. Sem semente aleatória, porque um teste que muda de
amostra a cada execução deixa de ser comparável entre rodadas.

Uso:
    python3 tools/make_model_probe.py --model artefatos/model_16.json --out build/cfg
"""
from __future__ import annotations

import argparse
import glob
import os
import struct

N_FEAT = 16
RECORD = struct.Struct("<I16f")
N_LINHAS = 2048


def carrega_brutas(dir_registros: str, quantas: int):
    """Amostra `quantas` linhas brutas, com passo fixo sobre todos os registros."""
    import numpy as np

    arquivos = sorted(glob.glob(os.path.join(dir_registros, "*.f32")))
    if not arquivos:
        raise SystemExit("nenhum .f32 em %s. Rode antes a paridade do dataset." % dir_registros)

    total = sum(os.path.getsize(p) // RECORD.size for p in arquivos)
    passo = max(1, total // quantas)

    linhas = []
    indice = 0
    for caminho in arquivos:
        with open(caminho, "rb") as fh:
            while len(linhas) < quantas:
                bloco = fh.read(RECORD.size * 8192)
                if not bloco:
                    break
                for rec in RECORD.iter_unpack(bloco):
                    if (indice % passo) == 0:
                        linhas.append(rec[1:])
                        if len(linhas) >= quantas:
                            break
                    indice += 1
        if len(linhas) >= quantas:
            break
    return np.asarray(linhas, dtype=np.float64), total


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--records", default="build/parity-dataset")
    ap.add_argument("--minmax", default="build/cfg/minmax.bin")
    ap.add_argument("--out", default="build/cfg")
    ap.add_argument("--rows", type=int, default=N_LINHAS)
    a = ap.parse_args()

    import numpy as np
    import xgboost as xgb

    brutas, total = carrega_brutas(a.records, a.rows)
    print("  %d linhas amostradas de %d, com passo fixo" % (brutas.shape[0], total))

    with open(a.minmax, "rb") as fh:
        (n,) = struct.unpack("<I", fh.read(4))
        if n != N_FEAT:
            raise SystemExit("minmax.bin declara %d features, esperado %d" % (n, N_FEAT))
        xmin = np.asarray(struct.unpack("<%dd" % N_FEAT, fh.read(8 * N_FEAT)), dtype=np.float64)
        xmax = np.asarray(struct.unpack("<%dd" % N_FEAT, fh.read(8 * N_FEAT)), dtype=np.float64)

    # A MESMA conta do extract_all: divisor 1,0 onde a feature e constante, e
    # nenhuma saturacao. Em float64, e so o resultado vira float32.
    rng = np.where(xmax > xmin, xmax - xmin, 1.0)
    norm = ((brutas - xmin) / rng).astype(np.float32)
    fora = int(((norm < 0.0) | (norm > 1.0)).sum())
    print("  %d valores fora de [0,1] nas linhas amostradas (a referencia nao satura)" % fora)

    bst = xgb.Booster()
    bst.load_model(a.model)
    probs = bst.predict(xgb.DMatrix(norm)).astype(np.float64)

    os.makedirs(a.out, exist_ok=True)
    norm.tofile(os.path.join(a.out, "model_rows.f32"))
    caminho = os.path.join(a.out, "model_probs.txt")
    with open(caminho, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# gerado por tools/make_model_probe.py; referencia = xgboost predict\n")
        fh.write("# linhas REAIS do dataset, normalizadas com os limites do treino\n")
        for linha in probs:
            fh.write(" ".join("%.17g" % v for v in linha) + "\n")

    classes = np.bincount(probs.argmax(1), minlength=5)
    print("  classes nas linhas de prova: %s" % classes.tolist())
    print("  -> %s e %s" % (os.path.join(a.out, "model_rows.f32"), caminho))


if __name__ == "__main__":
    main()
