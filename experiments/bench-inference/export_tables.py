#!/usr/bin/env python3
"""Exporta o modelo XGBoost para TABELAS binarias compactas.

A ideia a testar: o TL2cgen vira CODIGO e sofre pressao de cache de instrucoes,
enquanto aqui o modelo vira DADO -- um array de nos de 16 bytes, percorrido por
um laco fixo.

O tamanho desse codigo depende muito do modelo, e vale registrar os dois pontos
medidos, porque a diferenca entre eles e o argumento:

    300 rodadas x profundidade 8   323 mil linhas,  objeto de 11,7 MB
     25 rodadas x profundidade 8    48,6 mil linhas, objeto de  1,77 MB

O segundo e o modelo ADOTADO (24 329 nos, tabela de 0,39 MB). Foi com o primeiro
que a pressao de cache de instrucoes apareceu no bench; com o adotado o codigo
gerado e seis vezes menor e cabe muito melhor.

Layout de tables.bin (little-endian):
    int32  n_trees, n_class, n_nodes, n_feat
    float  base[n_class]               margem inicial por classe (base_score)
    int32  tree_root[n_trees]          indice do no raiz de cada arvore
    node   nodes[n_nodes]              16 bytes cada:
             float32 thr    (no interno: limiar | folha: valor da folha)
             int32   feat   (indice da feature | -1 se folha)
             int32   left   (filho "yes")
             int32   right  (filho "no")

Atencao ao base_score: neste modelo ele e um VETOR por classe (ex.:
[2.74, -1.00, -0.37, -0.75, -0.62]), nao um escalar. Logo ele NAO cancela no
softmax e precisa ser embarcado como margem inicial de cada classe.

Uso:
    python export_tables.py --model .../model_16.json --out build
"""
from __future__ import annotations

import argparse
import json
import os
import struct


def flatten(tree: dict, nodes: list) -> int:
    """Achata uma arvore (dict do dump JSON) no array global. Devolve o indice da raiz."""
    idx = len(nodes)
    if "leaf" in tree:
        nodes.append((float(tree["leaf"]), -1, 0, 0))
        return idx

    nodes.append(None)                       # reserva o lugar do no interno
    feat = int(str(tree["split"]).lstrip("f"))
    thr = float(tree["split_condition"])

    kids = {int(c["nodeid"]): c for c in tree["children"]}
    left = flatten(kids[int(tree["yes"])], nodes)     # x < thr
    right = flatten(kids[int(tree["no"])], nodes)     # x >= thr
    nodes[idx] = (thr, feat, left, right)
    return idx


def export(model_path: str, outdir: str, quiet: bool = False) -> int:
    """Exporta o modelo para <outdir>/tables.bin. Devolve o numero de nos."""
    os.makedirs(outdir, exist_ok=True)

    import xgboost as xgb
    bst = xgb.Booster()
    bst.load_model(model_path)

    cfg = json.loads(bst.save_config())
    n_class = int(cfg["learner"]["learner_model_param"].get("num_class", 1)) or 1
    n_feat = int(cfg["learner"]["learner_model_param"].get("num_feature", 0))

    # base_score pode vir escalar ("0.5") ou vetor por classe ("[a,b,c,...]").
    #
    # ⚠️ LIDO DO ARQUIVO DO MODELO, nao do save_config. Um xgboost mais ANTIGO que
    # o que treinou o modelo carrega o arquivo sem erro e devolve base_score = 0.5
    # pelo save_config, descartando o vetor que esta la dentro. Como aqui o
    # base_score e um vetor por classe, ele NAO cancela no softmax: as margens
    # iniciais sairiam erradas e nada falharia. Isso aconteceu de verdade, lendo
    # um modelo do xgboost 3.3.0 com o 2.1.3.
    raw = None
    try:
        with open(model_path, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
        raw = doc["learner"]["learner_model_param"].get("base_score")
        origem = "arquivo do modelo"
    except Exception:                                # noqa: BLE001
        raw = None
    if raw is None:
        raw = cfg["learner"]["learner_model_param"].get("base_score", "0")
        origem = "save_config (o arquivo nao pode ser lido como JSON)"

    raw = str(raw)
    if raw.strip().startswith("["):
        base = [float(v) for v in raw.strip()[1:-1].split(",") if v.strip()]
    else:
        base = [float(raw)] * n_class
    if len(base) != n_class:
        base = (base * n_class)[:n_class]

    dumps = bst.get_dump(dump_format="json")
    nodes: list = []
    roots: list = []
    for d in dumps:
        roots.append(flatten(json.loads(d), nodes))

    n_trees = len(roots)
    if not quiet:
        print(f"arvores={n_trees}  classes={n_class}  features={n_feat}")
        print(f"base_score={base}  (lido do {origem})")
        if len(set(base)) == 1:
            print("  (escalar: cancela no softmax)")
        else:
            print("  (VETOR por classe: NAO cancela no softmax, tem de ser embarcado)")
        print(f"nos={len(nodes)}  tabela={len(nodes) * 16 / 1e6:.2f} MB "
              f"(contra 11,7 MB de objeto do TL2cgen)")
        print(f"arvores por classe={n_trees // max(n_class, 1)}  (classe = indice % n_class)")

    path = os.path.join(outdir, "tables.bin")
    with open(path, "wb") as f:
        f.write(struct.pack("<4i", n_trees, n_class, len(nodes), n_feat))
        f.write(struct.pack(f"<{n_class}f", *base))
        f.write(struct.pack(f"<{n_trees}i", *roots))
        for thr, feat, left, right in nodes:
            f.write(struct.pack("<f3i", thr, feat, left, right))

    if not quiet:
        print(f"[ok] {path} ({os.path.getsize(path) / 1e6:.2f} MB)")
    return len(nodes)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--out", default="build")
    a = ap.parse_args()
    export(a.model, a.out)


if __name__ == "__main__":
    main()
