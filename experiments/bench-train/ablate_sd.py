#!/usr/bin/env python3
"""Qual feature de SD quebra o modelo treinado na bancada?

Com as 5 features sem conteúdo + as 5 de SD, o modelo treinado no KIM transfere para a
bancada com 0,10% de falso positivo, mas o mesmo conjunto treinado na BANCADA tem 19%
dentro do próprio domínio (contra 0,31% só com as 5). Este script extrai as features da
bancada uma única vez e treina variações: as 5, as 5 + cada feature de SD sozinha, e as
5 + todas, para achar a que causa o problema. Também mostra, para cada feature de SD, a
distribuição nos pacotes benignos do treino e do teste.

Uso (na imagem someip-parity:1):
    python3 experiments/bench-train/ablate_sd.py --repro /work/someip-ids-multiclass-contentext \\
        --runs experiments/bench-train/runs
"""
import argparse
import os
import sys
import types

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import train_bench as tb  # noqa: E402

BASE = ["ip_time_interval", "ip_len_change", "l4_len_change", "someip_len", "l4_len"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repro", required=True)
    ap.add_argument("--runs", required=True)
    ap.add_argument("--n-jobs", type=int, default=4)
    a = ap.parse_args()
    sys.path.insert(0, os.path.join(a.repro, "src"))
    import someip
    import extract
    import extract_ext
    import bytemodel
    import pipeline
    ref = types.SimpleNamespace(someip=someip, _ts=extract._ts, L=extract.L, ByteModel=bytemodel.ByteModel,
                                FeatureStreamer=extract_ext.FeatureStreamer, sd=True)
    nomes = list(extract_ext.FEATURE_NAMES) + tb.SD_NOMES

    def junta(split):
        itens = [(os.path.join(a.runs, "%s-%s" % (split, s), "scn.pcap"),
                  tb.le_macs(os.path.join(a.runs, "%s-%s" % (split, s), "injector.mac"))) for s in tb.CENARIOS]
        return itens

    cache = os.path.join(a.runs, "features-sd.npz")
    if os.path.exists(cache):
        d = np.load(cache)
        Xtr, ytr, Xte, yte = d["Xtr"], d["ytr"], d["Xte"], d["yte"]
        print("features do cache %s" % cache)
    else:
        bm = tb.ajusta_bytemodels(junta("treino"), ref)
        tr = [tb.extrai(p, m, bm, ref) for p, m in junta("treino")]
        te = [tb.extrai(p, m, bm, ref) for p, m in junta("teste")]
        Xtr = np.concatenate([x for x, _ in tr]); ytr = np.concatenate([y for _, y in tr])
        Xte = np.concatenate([x for x, _ in te]); yte = np.concatenate([y for _, y in te])
        np.savez_compressed(cache, Xtr=Xtr, ytr=ytr, Xte=Xte, yte=yte)

    print("distribuição das features de SD nos pacotes BENIGNOS (média / p99 / máx):")
    for n in tb.SD_NOMES:
        i = nomes.index(n)
        for rot, X, y in (("treino", Xtr, ytr), ("teste", Xte, yte)):
            v = X[y == 0, i]
            print("  %-22s %-6s %9.3f %9.3f %9.3f" % (n, rot, v.mean(), np.percentile(v, 99), v.max()))
        va = Xtr[ytr != 0, i]
        print("  %-22s %-6s %9.3f %9.3f %9.3f" % (n, "ataque", va.mean(), np.percentile(va, 99), va.max()))

    variantes = [("5 sem conteúdo", BASE, None)]
    variantes += [("5 + " + n, BASE + [n], None) for n in tb.SD_NOMES]
    variantes += [("5 + todas de SD", BASE + tb.SD_NOMES, None)]
    # Com pesos por classe: a correção do desequilíbrio (Fuzzy com 435 pacotes no treino).
    for pot in (0.5, 1.0):
        variantes += [("5 sem conteúdo, pesos^%.1f" % pot, BASE, pot),
                      ("5 + sd_offer_rate_src, pesos^%.1f" % pot, BASE + ["sd_offer_rate_src"], pot),
                      ("5 + todas de SD, pesos^%.1f" % pot, BASE + tb.SD_NOMES, pot)]
    print("\n%-40s %8s   %s" % ("variante (treino e teste na bancada)", "FP", "recall DoS / Fuzzy / MITM s / MITM m"))
    for nome, cols_n, pot in variantes:
        cols = [nomes.index(n) for n in cols_n]
        mm = (Xtr[:, cols].min(0), Xtr[:, cols].max(0))
        w = tb.pesos_por_classe(ytr, pot) if pot is not None else None
        clf = tb.treina(tb.normaliza(Xtr[:, cols], mm), ytr, pipeline.model_params(n_jobs=a.n_jobs), w)
        r = tb.avalia(clf, tb.normaliza(Xte[:, cols], mm), yte)
        rec = " / ".join("%.2f" % r["por_classe"][c]["recall"] if c in r["por_classe"] and r["por_classe"][c]["recall"] is not None
                         else "-" for c in tb.CLASSES[1:])
        print("%-40s %7.3f%%   %s" % (nome, 100 * r["falso_positivo"], rec))


if __name__ == "__main__":
    main()
