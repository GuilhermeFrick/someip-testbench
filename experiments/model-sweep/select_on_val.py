#!/usr/bin/env python3
"""Selecao de hiperparametros em VALIDACAO, nao no teste.

A varredura anterior comparou configuracoes pelo macro-F1 no conjunto de TESTE,
que e o mesmo sobre o qual o artigo reporta. Isso e selecao no teste: o
procedimento e atacavel mesmo que o resultado seja robusto.

Aqui o split e aninhado, preservando a ordem temporal dentro de cada arquivo:

    |<--------- treino (70%) --------->|<---- teste (30%) ---->|
    |<-- treino interno -->|<-- val -->|
            (70% de 70%)     (30% de 70%)

Fluxo honesto: varre na validacao, escolhe ali, retreina o vencedor no treino
COMPLETO e so entao avalia no teste, que nunca participou da escolha.

Uso:
    python select_on_val.py --repo ~/someip-ids-multiclass-contentext --out val
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

import numpy as np

ROUNDS = [25, 50, 100, 200, 300]
DEPTHS = [4, 6, 8]
BASELINE = (300, 8)       # a configuracao que o artigo usava antes


def nested_split(counts, frac_test=0.70, frac_val=0.70):
    """Devolve (treino_interno, validacao, treino_completo, teste), temporais."""
    inner, val, full, test = [], [], [], []
    s = 0
    for c in counts:
        cut_te = s + int(c * frac_test)          # fim do treino completo
        cut_va = s + int(c * frac_test * frac_val)  # fim do treino interno
        inner.append(np.arange(s, cut_va))
        val.append(np.arange(cut_va, cut_te))
        full.append(np.arange(s, cut_te))
        test.append(np.arange(cut_te, s + c))
        s += c
    return (np.concatenate(inner), np.concatenate(val),
            np.concatenate(full), np.concatenate(test))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--out", default="val")
    ap.add_argument("--cache", default="features.npz")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    sys.path.insert(0, os.path.join(a.repo, "src"))
    import pipeline
    from sklearn.metrics import classification_report
    from xgboost import XGBClassifier

    z = np.load(a.cache, allow_pickle=True)
    Xn, y, counts = z["Xn"], z["y"], z["counts"].tolist()
    idx = pipeline.CONTENT_EXT
    cls = pipeline.CLASSES

    inner, val, full, test = nested_split(counts)
    print(f"[split] treino interno={len(inner):,}  validacao={len(val):,}  "
          f"treino completo={len(full):,}  teste={len(test):,}")

    def fit_eval(tr_idx, ev_idx, rounds, depth):
        clf = XGBClassifier(objective="multi:softprob", num_class=pipeline.N,
                            n_estimators=rounds, max_depth=depth,
                            learning_rate=0.3, tree_method="hist", device="cpu",
                            max_bin=256, n_jobs=-1, eval_metric="mlogloss")
        clf.fit(Xn[np.ix_(tr_idx, idx)], y[tr_idx])
        rep = classification_report(y[ev_idx], clf.predict(Xn[np.ix_(ev_idx, idx)]),
                                    target_names=cls, digits=4, output_dict=True,
                                    zero_division=0)
        return clf, rep

    # ---- 1. varredura NA VALIDACAO ----
    print("\n=== selecao na validacao ===")
    val_rows = []
    for depth in DEPTHS:
        for rounds in ROUNDS:
            t0 = time.time()
            _, rep = fit_eval(inner, val, rounds, depth)
            m = rep["macro avg"]["f1-score"]
            val_rows.append({"rounds": rounds, "depth": depth, "macro_f1_val": float(m),
                             "mitm_multi_val": float(rep["mitm_multi"]["f1-score"])})
            print(f"  r{rounds:<3d} d{depth}  macro-F1(val)={m:.4f}  "
                  f"mitm_multi={rep['mitm_multi']['f1-score']:.4f}  ({time.time()-t0:.0f}s)",
                  flush=True)

    best = max(val_rows, key=lambda r: r["macro_f1_val"])
    print(f"\n[vencedor na validacao] {best['rounds']} rodadas, profundidade "
          f"{best['depth']}  (macro-F1 val {best['macro_f1_val']:.4f})")

    # ---- 2. retreina no treino COMPLETO e avalia no TESTE ----
    print("\n=== avaliacao no teste (nunca usado na escolha) ===")
    final = {}
    for tag, (r, d) in {"escolhido": (best["rounds"], best["depth"]),
                        "baseline_anterior": BASELINE}.items():
        _, rep = fit_eval(full, test, r, d)
        final[tag] = {"rounds": r, "depth": d,
                      "macro_f1_test": float(rep["macro avg"]["f1-score"]),
                      "f1": {c: float(rep[c]["f1-score"]) for c in cls}}
        print(f"  {tag:18s} r{r} d{d}  macro-F1(teste)={final[tag]['macro_f1_test']:.4f}  "
              f"mitm_multi={rep['mitm_multi']['f1-score']:.4f}")

    json.dump({"validacao": val_rows, "teste": final},
              open(os.path.join(a.out, "selecao.json"), "w"), indent=2)
    print(f"\n[ok] {a.out}/selecao.json")


if __name__ == "__main__":
    main()
