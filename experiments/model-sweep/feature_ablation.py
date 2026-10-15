#!/usr/bin/env python3
"""Ablacao de features no modelo novo, com repeticao de semente.

Tres perguntas numa execucao:
  1. A historia 12 -> 16 da C1 se mantem no modelo encolhido? (subconjunto base12)
  2. Da para remover features sem perder classe, agora que a extracao domina?
  3. A vantagem de r50_d4 sobre r25_d8 e sinal ou ruido? (3 sementes)

Reporta macro-F1 E F1 por classe: ganho global esconde criticidade por classe,
que foi a licao da source diversity.

Uso:
    python feature_ablation.py --repo ~/someip-ids-multiclass-contentext --out abl
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

import numpy as np

# indice da feature -> categoria, e custo MEDIDO por categoria (us/pacote).
# Fonte: experiments/bench-inference, decomposicao incremental no EPYC.
CAT_OF = {0: "time_interval",
          1: "likelihood", 2: "likelihood", 3: "likelihood",
          4: "entropy", 5: "entropy", 6: "entropy",
          7: "changes", 8: "changes", 9: "changes",
          10: "length", 11: "length",
          12: "repetition",
          13: "abs_size", 14: "abs_size",
          15: "src_diversity"}
CAT_US = {"parse": 2.350, "time_interval": 0.511, "likelihood": 3.553,
          "entropy": 3.320, "changes": 2.290, "length": 0.085,
          "repetition": 0.550, "abs_size": 0.114, "src_diversity": 9.132}

ALL16 = list(range(16))
BASE12 = list(range(12))

# Subconjuntos a testar. A categoria so custa se ao menos um indice dela estiver
# presente, entao remover uma feature de uma categoria de 3 nao economiza nada.
SUBSETS = {
    "base12":        BASE12,                              # o baseline do Kim
    "all16":         ALL16,                               # o conjunto atual
    "no_srcdiv":     [i for i in ALL16 if i != 15],       # tira a mais cara
    "no_lowgain":    [i for i in ALL16 if i not in (0, 10, 11)],
    "base12_rep":    BASE12 + [12],                       # baseline + repeticao
    "lean":          [i for i in ALL16 if i not in (0, 10, 11, 15)],
}

# (nome, rodadas, profundidade)
MODELS = [("r50_d4", 50, 4), ("r25_d8", 25, 8)]
SEEDS = [0, 1, 2]


def extraction_us(idx) -> float:
    cats = {CAT_OF[i] for i in idx}
    return CAT_US["parse"] + sum(CAT_US[c] for c in cats)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--out", default="abl")
    ap.add_argument("--cache", default="features.npz")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    sys.path.insert(0, os.path.join(a.repo, "src"))
    import pipeline
    from sklearn.metrics import classification_report
    from xgboost import XGBClassifier

    z = np.load(a.cache, allow_pickle=True)      # cache da varredura anterior
    Xn, y, counts = z["Xn"], z["y"], z["counts"]
    itr, ite = pipeline.temporal_split(counts.tolist())
    cls = pipeline.CLASSES
    print(f"[split] treino={len(itr):,} teste={len(ite):,}")

    rows = []
    for mname, rounds, depth in MODELS:
        # so o conjunto completo precisa das duas profundidades; o resto usa r50_d4
        subs = SUBSETS.items() if mname == "r50_d4" else [("all16", ALL16)]
        for sname, idx in subs:
            for seed in SEEDS:
                t0 = time.time()
                clf = XGBClassifier(objective="multi:softprob", num_class=pipeline.N,
                                    n_estimators=rounds, max_depth=depth,
                                    learning_rate=0.3, tree_method="hist",
                                    device="cpu", max_bin=256, n_jobs=-1,
                                    random_state=seed, eval_metric="mlogloss")
                clf.fit(Xn[np.ix_(itr, idx)], y[itr])
                pred = clf.predict(Xn[np.ix_(ite, idx)])
                rep = classification_report(y[ite], pred, target_names=cls,
                                            digits=4, output_dict=True, zero_division=0)
                rows.append({"model": mname, "subset": sname, "seed": seed,
                             "n_feat": len(idx),
                             "macro_f1": float(rep["macro avg"]["f1-score"]),
                             "extracao_us": round(extraction_us(idx), 3),
                             "f1": {c: float(rep[c]["f1-score"]) for c in cls},
                             "train_s": round(time.time() - t0, 1)})
                r = rows[-1]
                print(f"  {mname:8s} {sname:12s} seed={seed} "
                      f"macro-F1={r['macro_f1']:.4f} mitm_multi={r['f1']['mitm_multi']:.4f} "
                      f"extracao={r['extracao_us']:5.2f}us", flush=True)

    json.dump(rows, open(os.path.join(a.out, "ablation.json"), "w"), indent=2)
    print(f"\n[ok] {len(rows)} execucoes em {a.out}/ablation.json")


if __name__ == "__main__":
    main()
