#!/usr/bin/env python3
"""Seleção de hiperparâmetros em VALIDAÇÃO, com split temporal aninhado.

===============================================================================
O PROBLEMA QUE ESTE SCRIPT RESOLVE
===============================================================================

Uma varredura anterior (`sweep.py`) comparou 15 configurações de hiperparâmetros
pelo macro-F1 do conjunto de **teste** — o mesmo conjunto sobre o qual o artigo
reporta o resultado final.

Isso é *seleção no teste*. O problema não é o resultado ser ruim; é o
procedimento não permitir distinguir mérito de sorte. Ao escolher, entre 15
candidatos, o que teve o melhor número no conjunto de reporte, parte do que se
mede vira ruído daquele conjunto específico, e a métrica publicada fica
otimista por construção.

No nosso caso isso não foi hipotético. A configuração eleita naquela varredura
(50 rodadas, profundidade 4) tinha:

    - o MELHOR macro-F1 de teste    (0,9695, 1º lugar entre 15)
    - a SÉTIMA melhor validação     (0,9418)

Ou seja: vantagem concentrada justamente no conjunto onde se reporta. A seleção
honesta elege outra configuração.

===============================================================================
O MÉTODO
===============================================================================

Split aninhado, preservando a ordem temporal dentro de cada arquivo de captura:

    |<--------------- treino (70%) --------------->|<---- teste (30%) ---->|
    |<-- treino interno (49%) -->|<-- val (21%) -->|
                                                    ^
                                        o teste nunca é tocado na escolha

A ordem temporal importa porque o tráfego tem deriva: o conjunto de teste é um
período POSTERIOR ao de treino. Um split aleatório deixaria o modelo ver o
futuro e esconderia o sobreajuste. A validação reproduz essa mesma relação
dentro do treino, para que a escolha seja feita sob a condição em que o modelo
vai ser julgado.

Fluxo em três etapas:

    1. VARRER    treina cada configuração no treino interno,
                 avalia na validação
    2. ESCOLHER  seleciona a de maior macro-F1 na validação
    3. REPORTAR  retreina a vencedora no treino COMPLETO (treino interno + val)
                 e só então avalia no teste

A etapa 3 retreina no treino completo porque, uma vez escolhida a configuração,
não há motivo para desperdiçar os 21% de dados que serviram de validação.

===============================================================================
SAÍDA
===============================================================================

`<out>/selecao.json`, com:
    "validacao" : as 15 configurações e seu macro-F1 de validação
    "teste"     : a vencedora e o baseline anterior, avaliados no teste

Gráficos: `python plot_selecao.py` (ver RESULTADOS.md).

Uso:
    python select_on_val.py --repo <repo-do-ids> --out val
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

import numpy as np

# Grade da varredura. `rounds` é o número de rodadas de boosting; com 5 classes,
# o XGBoost constrói rounds x 5 árvores no total.
ROUNDS = [25, 50, 100, 200, 300]
DEPTHS = [4, 6, 8]

# Configuração usada no artigo antes desta varredura, mantida como referência
# para quantificar o ganho.
BASELINE = (300, 8)


def nested_split(counts, frac_test=0.70, frac_val=0.70):
    """Corta treino interno, validação, treino completo e teste, nessa ordem.

    `counts` é o número de pacotes de cada arquivo de captura. O corte é feito
    DENTRO de cada arquivo e em sequência, nunca embaralhando: assim a validação
    é sempre um período posterior ao do treino interno, e o teste posterior a
    ambos. É isso que preserva a ordem temporal.

    Frações resultantes do total, com os padrões:
        treino interno  49%   (70% de 70%)
        validação       21%   (30% de 70%)
        treino completo 70%   (interno + validação)
        teste           30%
    """
    inner, val, full, test = [], [], [], []
    s = 0
    for c in counts:
        cut_te = s + int(c * frac_test)              # fim do treino completo
        cut_va = s + int(c * frac_test * frac_val)   # fim do treino interno
        inner.append(np.arange(s, cut_va))
        val.append(np.arange(cut_va, cut_te))
        full.append(np.arange(s, cut_te))
        test.append(np.arange(cut_te, s + c))
        s += c
    return (np.concatenate(inner), np.concatenate(val),
            np.concatenate(full), np.concatenate(test))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True, help="repo do IDS multiclasse")
    ap.add_argument("--out", default="val")
    ap.add_argument("--cache", default="features.npz",
                    help="features ja extraidas; a extracao NAO depende dos "
                         "hiperparametros, entao roda uma vez so")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    sys.path.insert(0, os.path.join(a.repo, "src"))
    import pipeline
    from sklearn.metrics import classification_report
    from xgboost import XGBClassifier

    z = np.load(a.cache, allow_pickle=True)
    Xn, y, counts = z["Xn"], z["y"], z["counts"].tolist()
    idx = pipeline.CONTENT_EXT          # as 16 features
    cls = pipeline.CLASSES

    inner, val, full, test = nested_split(counts)
    print(f"[split] treino interno={len(inner):,}  validacao={len(val):,}  "
          f"treino completo={len(full):,}  teste={len(test):,}")

    def fit_eval(tr_idx, ev_idx, rounds, depth):
        """Treina em `tr_idx` e avalia em `ev_idx`. Demais hiperparâmetros fixos."""
        clf = XGBClassifier(objective="multi:softprob", num_class=pipeline.N,
                            n_estimators=rounds, max_depth=depth,
                            learning_rate=0.3, tree_method="hist", device="cpu",
                            max_bin=256, n_jobs=-1, eval_metric="mlogloss")
        clf.fit(Xn[np.ix_(tr_idx, idx)], y[tr_idx])
        rep = classification_report(y[ev_idx], clf.predict(Xn[np.ix_(ev_idx, idx)]),
                                    target_names=cls, digits=4, output_dict=True,
                                    zero_division=0)
        return clf, rep

    # ---------- ETAPA 1: varrer, avaliando sempre na VALIDAÇÃO ----------
    print("\n=== etapa 1: varredura na validacao ===")
    val_rows = []
    for depth in DEPTHS:
        for rounds in ROUNDS:
            t0 = time.time()
            _, rep = fit_eval(inner, val, rounds, depth)
            m = rep["macro avg"]["f1-score"]
            val_rows.append({"rounds": rounds, "depth": depth,
                             "macro_f1_val": float(m),
                             "mitm_multi_val": float(rep["mitm_multi"]["f1-score"])})
            print(f"  r{rounds:<3d} d{depth}  macro-F1(val)={m:.4f}  "
                  f"mitm_multi={rep['mitm_multi']['f1-score']:.4f}  "
                  f"({time.time()-t0:.0f}s)", flush=True)

    # ---------- ETAPA 2: escolher pela VALIDAÇÃO, e só por ela ----------
    # Critério único: maior macro-F1 de validação. Tamanho do modelo e latência
    # NÃO entram aqui; são discutidos à parte, com os números de custo medidos.
    best = max(val_rows, key=lambda r: r["macro_f1_val"])
    print(f"\n[vencedor na validacao] {best['rounds']} rodadas, profundidade "
          f"{best['depth']}  (macro-F1 val {best['macro_f1_val']:.4f})")

    # ---------- ETAPA 3: retreinar no treino completo e reportar no TESTE ----------
    # Primeiro contato do teste com qualquer decisão. O baseline anterior entra
    # aqui apenas para quantificar o ganho, não para influenciar a escolha.
    print("\n=== etapa 3: avaliacao no teste (nao participou da escolha) ===")
    final = {}
    for tag, (r, d) in {"escolhido": (best["rounds"], best["depth"]),
                        "baseline_anterior": BASELINE}.items():
        _, rep = fit_eval(full, test, r, d)
        final[tag] = {"rounds": r, "depth": d,
                      "macro_f1_test": float(rep["macro avg"]["f1-score"]),
                      "f1": {c: float(rep[c]["f1-score"]) for c in cls}}
        print(f"  {tag:18s} r{r} d{d}  "
              f"macro-F1(teste)={final[tag]['macro_f1_test']:.4f}  "
              f"mitm_multi={rep['mitm_multi']['f1-score']:.4f}")

    json.dump({"validacao": val_rows, "teste": final},
              open(os.path.join(a.out, "selecao.json"), "w"), indent=2)
    print(f"\n[ok] {a.out}/selecao.json")


if __name__ == "__main__":
    main()
