#!/usr/bin/env python3
"""Converte o `bytemodels.npz` do treino para o formato que o `ids::ByteModels` lê.

Formato, little-endian, três tabelas na ordem someip, sd, l4. Cada uma: `uint32`
com o número de posições, seguido de `posições × 256` valores `double`.

As tabelas são POSICIONAIS: a log-probabilidade de cada valor de byte depende de
onde o byte está no payload. L vale 64 para someip, 80 para sd e 64 para l4, e
esses números são contrato com o treino — o `ByteModels::Create` os confere
contra o que a build espera em vez de aceitar o que vier no arquivo.

Até agora quem exportava este artefato eram os geradores de teste
(`tests/gen_parity_*.py`), que montam tabelas sintéticas. Este converte a tabela
REAL, ajustada no tráfego benigno pelo `fit_bytemodels`.

Uso:
    python3 tools/export_bytemodels.py --npz artefatos/bytemodels.npz \\
            --out build/cfg/bytemodels.bin
"""
from __future__ import annotations

import argparse
import os
import struct

# Contrato com o treino, igual ao do src/ids_sensor/ByteModel.hpp.
POSICOES = {"someip": 64, "sd": 80, "l4": 64}
ORDEM = ("someip", "sd", "l4")
N_VALORES = 256


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--npz", required=True, help="bytemodels.npz salvo pelo pipeline de treino")
    ap.add_argument("--out", default="build/cfg/bytemodels.bin")
    a = ap.parse_args()

    import numpy as np

    z = np.load(a.npz)
    faltando = [k for k in ORDEM if k not in z.files]
    if faltando:
        raise SystemExit("faltam tabelas no npz: %s (tem: %s)" % (", ".join(faltando), ", ".join(z.files)))

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "wb") as fh:
        for nome in ORDEM:
            tabela = z[nome]
            esperado = (POSICOES[nome], N_VALORES)
            if tabela.shape != esperado:
                # Recusar aqui e melhor do que gravar um artefato que o lado C++
                # vai ler com o numero errado de posicoes.
                raise SystemExit("tabela %s com forma %s, esperado %s" % (nome, tabela.shape, esperado))
            fh.write(struct.pack("<I", POSICOES[nome]))
            # float64 nativo, little-endian, linha a linha: e o que o
            # ByteModels::Create le com fread direto no array.
            fh.write(np.ascontiguousarray(tabela, dtype="<f8").tobytes())
            print("  %-7s %s  min=%.6g  max=%.6g" % (nome, tabela.shape, float(tabela.min()), float(tabela.max())))

    print("%d bytes -> %s" % (os.path.getsize(a.out), a.out))


if __name__ == "__main__":
    main()
