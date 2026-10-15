#!/usr/bin/env python3
"""Varredura de encolhimento do modelo: macro-F1 x custo de inferencia.

Metodologia inspirada em Dal Zotto & Moraes (SBCCI 2025), que encolhem o modelo
XGBoost iterativamente (menos arvores, menos profundidade) aceitando uma perda
limitada da metrica, para tornar a inferencia barata o bastante para embarcar.

Aqui o alvo e diferente (classificacao multiclasse, nao regressao), entao a
metrica e o macro-F1, e o custo e medido em nos percorridos e em latencia real
do avaliador compacto.

A extracao do dataset e cara, entao e feita UMA vez e cacheada em .npz.

Uso:
    python sweep.py --repo ~/someip-ids-multiclass-contentext --out sweep
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

import numpy as np

# grade: rodadas de boosting x profundidade. 300x8 e o modelo atual do artigo.
ROUNDS = [25, 50, 100, 200, 300]
DEPTHS = [4, 6, 8]


def load_features(repo: str, cache: str):
    """Extrai o dataset uma vez e cacheia (a extracao e o passo caro)."""
    if os.path.exists(cache):
        print(f"[cache] usando {cache}")
        z = np.load(cache, allow_pickle=True)
        return z["Xn"], z["y"], z["counts"]

    sys.path.insert(0, os.path.join(repo, "src"))
    import pipeline
    print("[extracao] dataset completo (demora alguns minutos)...", flush=True)
    t0 = time.time()
    Xn, y, counts, _t, _m, _mm = pipeline.extract_all(
        os.path.join(repo, "data", "pcap"), max_packets=None)
    print(f"[extracao] {time.time() - t0:.0f}s, X={Xn.shape}")
    np.savez_compressed(cache, Xn=Xn, y=y, counts=np.array(counts, dtype=object))
    return Xn, y, np.array(counts, dtype=object)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True, help="repo do IDS multiclasse")
    ap.add_argument("--out", default="sweep")
    ap.add_argument("--cache", default="features.npz")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    sys.path.insert(0, os.path.join(a.repo, "src"))
    import pipeline
    from sklearn.metrics import classification_report
    from xgboost import XGBClassifier

    # o exportador de tabelas vive no experimento vizinho
    sys.path.insert(0, os.path.abspath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bench-inference")))
    import export_tables as ET

    Xn, y, counts = load_features(a.repo, a.cache)
    itr, ite = pipeline.temporal_split(counts.tolist())
    idx = pipeline.CONTENT_EXT                      # as 16 features
    Xtr, ytr = Xn[np.ix_(itr, idx)], y[itr]
    Xte, yte = Xn[np.ix_(ite, idx)], y[ite]
    n_class = pipeline.N
    print(f"[split] treino={len(itr):,} teste={len(ite):,} classes={n_class}")

    results = []
    for depth in DEPTHS:
        for rounds in ROUNDS:
            name = f"r{rounds}_d{depth}"
            t0 = time.time()
            clf = XGBClassifier(objective="multi:softprob", num_class=n_class,
                                n_estimators=rounds, max_depth=depth,
                                learning_rate=0.3, tree_method="hist",
                                device="cpu", max_bin=256, n_jobs=-1,
                                eval_metric="mlogloss")
            clf.fit(Xtr, ytr)
            train_s = time.time() - t0

            pred = clf.predict(Xte)
            rep = classification_report(yte, pred, target_names=pipeline.CLASSES,
                                        digits=4, output_dict=True, zero_division=0)
            macro = rep["macro avg"]["f1-score"]

            mdir = os.path.join(a.out, name)
            os.makedirs(mdir, exist_ok=True)
            mpath = os.path.join(mdir, "model.json")
            clf.get_booster().save_model(mpath)

            # exporta a tabela compacta e conta os nos (o custo estrutural)
            n_nodes = ET.export(mpath, mdir, quiet=True)

            row = {"name": name, "rounds": rounds, "depth": depth,
                   "macro_f1": float(macro), "n_nodes": int(n_nodes),
                   "n_trees": rounds * n_class,
                   "tabela_mb": n_nodes * 16 / 1e6,
                   "train_s": round(train_s, 1),
                   "f1_por_classe": {c: float(rep[c]["f1-score"])
                                     for c in pipeline.CLASSES}}
            results.append(row)
            print(f"  {name:10s} macro-F1={macro:.4f}  nos={n_nodes:7,d}  "
                  f"tabela={row['tabela_mb']:5.2f} MB  treino={train_s:5.1f}s", flush=True)

    json.dump(results, open(os.path.join(a.out, "sweep_metrics.json"), "w"), indent=2)
    print(f"\n[ok] {len(results)} configuracoes em {a.out}/sweep_metrics.json")


if __name__ == "__main__":
    main()
