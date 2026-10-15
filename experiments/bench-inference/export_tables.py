#!/usr/bin/env python3
"""Exporta o modelo XGBoost para TABELAS binarias compactas.

A ideia a testar: o TL2cgen vira megabytes de CODIGO (323 mil linhas de if/else,
objeto de 11,7 MB) e sofre pressao de cache de instrucoes. Aqui o modelo vira
DADO: um array de nos de 16 bytes, percorrido por um laco fixo.

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


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--out", default="build")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    import xgboost as xgb
    bst = xgb.Booster()
    bst.load_model(a.model)

    cfg = json.loads(bst.save_config())
    n_class = int(cfg["learner"]["learner_model_param"].get("num_class", 1)) or 1
    n_feat = int(cfg["learner"]["learner_model_param"].get("num_feature", 0))
    # base_score pode vir escalar ("0.5") ou vetor por classe ("[a,b,c,...]")
    raw = str(cfg["learner"]["learner_model_param"].get("base_score", "0"))
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
    print(f"arvores={n_trees}  classes={n_class}  features={n_feat}  base_score={base}")
    print(f"nos={len(nodes)}  tabela={len(nodes) * 16 / 1e6:.2f} MB "
          f"(contra 11,7 MB de objeto do TL2cgen)")
    print(f"arvores por classe={n_trees // max(n_class, 1)}  (classe = indice % n_class)")

    path = os.path.join(a.out, "tables.bin")
    with open(path, "wb") as f:
        f.write(struct.pack("<4i", n_trees, n_class, len(nodes), n_feat))
        f.write(struct.pack(f"<{n_class}f", *base))
        f.write(struct.pack(f"<{n_trees}i", *roots))
        for thr, feat, left, right in nodes:
            f.write(struct.pack("<f3i", thr, feat, left, right))

    print(f"[ok] {path} ({os.path.getsize(path) / 1e6:.2f} MB)")


if __name__ == "__main__":
    main()
