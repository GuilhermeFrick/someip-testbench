#!/usr/bin/env python3
"""Treina um modelo DESCARTÁVEL, só para verificar a ABI do C gerado pelo TL2cgen.

Por que existe: o `ids::Tl2cgenClassifier` tem de declarar `union Entry` e a
assinatura de `predict` para não depender do `header.h` gerado, que só existe
depois do passo do TL2cgen. Errar o layout dessa union não dá erro de
compilação — dá corrupção silenciosa dos valores das features. Então o layout é
**conferido contra o header real**, não suposto.

De quebra, o modelo serve de oráculo cruzado: TL2cgen e o avaliador compacto
(`tables.bin`) têm de concordar sobre as mesmas linhas, e os dois têm de
concordar com o `predict_proba` do Python.

⚠️ O modelo NÃO tem valor nenhum: é treinado em dados aleatórios. Ele tem a
MESMA FORMA do modelo adotado (16 features, 5 classes, 25 rodadas,
profundidade 8, `multi:softprob`) porque é a forma que determina a ABI e o
layout das tabelas, não o conteúdo.

Uso:
    python3 experiments/bench-inference/make_probe_model.py --out build/probe
"""
from __future__ import annotations

import argparse
import json
import os

N_FEAT = 16
N_CLASS = 5
N_ROUNDS = 25
DEPTH = 8
N_ROWS = 4000
N_PROBE = 256
SEED = 20261101


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="build/probe")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    import numpy as np
    import xgboost as xgb

    rng = np.random.default_rng(SEED)
    # Dados aleatórios, mas com um sinal fraco plantado, para a árvore ter o que
    # cortar: sem nenhum sinal o modelo degenera em folhas iguais e um avaliador
    # errado passaria o teste por acidente.
    X = rng.normal(size=(N_ROWS, N_FEAT)).astype(np.float32)
    y = ((X[:, 0] > 0).astype(int) + 2 * (X[:, 3] > 0.5).astype(int)) % N_CLASS

    # Os mesmos hiperparâmetros da fonte única do repo de reprodução
    # (pipeline.model_params), porque é a forma que importa aqui.
    #
    # Pela API nativa (xgb.train), não pelo XGBClassifier: o wrapper sklearn
    # exige o scikit-learn instalado, e trazer esse pacote para dentro da imagem
    # só para treinar um modelo descartável não se justifica. O artefato salvo é
    # o mesmo Booster nos dois caminhos, que é o que o TL2cgen lê.
    params = dict(objective="multi:softprob", num_class=N_CLASS,
                  max_depth=DEPTH, learning_rate=0.3, tree_method="hist",
                  max_bin=256, nthread=1, eval_metric="mlogloss")
    dtrain = xgb.DMatrix(X, label=y)
    bst = xgb.train(params, dtrain, num_boost_round=N_ROUNDS)

    modelo = os.path.join(a.out, "probe_model.json")
    bst.save_model(modelo)
    print("[1/3] modelo -> %s" % modelo)

    # Linhas de prova e as probabilidades de referência do Python. Com
    # multi:softprob o predict já devolve probabilidades por classe.
    Xp = rng.normal(size=(N_PROBE, N_FEAT)).astype(np.float32)
    P = bst.predict(xgb.DMatrix(Xp)).astype(np.float64)
    Xp.tofile(os.path.join(a.out, "probe_rows.f32"))
    with open(os.path.join(a.out, "probe_probs.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# gerado por make_probe_model.py; referencia = xgboost predict_proba\n")
        fh.write("# uma linha por amostra: %d probabilidades em %%.17g\n" % N_CLASS)
        for linha in P:
            fh.write(" ".join("%.17g" % v for v in linha) + "\n")
    print("[2/3] %d linhas de prova e as probabilidades de referencia" % N_PROBE)

    cfg = json.loads(bst.save_config())
    bruto = str(cfg["learner"]["learner_model_param"].get("base_score", "0"))
    print("[3/3] base_score cru do modelo: %s" % bruto)
    print("      (se vier entre colchetes, e VETOR por classe e nao cancela no softmax)")


if __name__ == "__main__":
    main()
