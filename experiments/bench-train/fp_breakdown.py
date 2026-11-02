#!/usr/bin/env python3
"""De onde vem o falso positivo de cada modelo, por rodada de teste e por origem.

Complementa train_bench.py. O modelo treinado na bancada ainda tem ~5% de falso
positivo dentro do próprio domínio, e há duas explicações candidatas:
  (a) conteúdo: o payload `counter` nunca se repete, então todo valor é novo;
  (b) reação ao ataque: quadros benignos pelo MAC (vítimas assinando no atacante,
      reconectando) que só existem por causa do ataque.
Separar a rodada só benigna das rodadas com ataque, e o tipo de quadro (SD ou não,
serviço, origem), distingue as duas.

Uso (na imagem someip-parity:1):
    python3 experiments/bench-train/fp_breakdown.py --repro /work/someip-ids-multiclass-contentext \\
        --runs experiments/bench-train/runs --kim-art /work/artefatos --bench-art experiments/bench-train/out
"""
import argparse
import os
import sys
from collections import Counter

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import train_bench as tb  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repro", required=True)
    ap.add_argument("--runs", required=True)
    ap.add_argument("--kim-art", required=True)
    ap.add_argument("--bench-art", required=True)
    a = ap.parse_args()

    sys.path.insert(0, os.path.join(a.repro, "src"))
    import types
    import someip
    import extract
    import extract_ext
    import bytemodel
    import xgboost as xgb
    ref = types.SimpleNamespace(someip=someip, _ts=extract._ts, L=extract.L, ByteModel=bytemodel.ByteModel,
                                FeatureStreamer=extract_ext.FeatureStreamer)

    modelos = {}
    for nome, d in (("kim", a.kim_art), ("bancada", a.bench_art)):
        mm = np.load(os.path.join(d, "minmax.npy"))
        modelos[nome] = (xgb.Booster(model_file=os.path.join(d, "model_16.json")),
                         tb.carrega_bytemodels(os.path.join(d, "bytemodels.npz"), ref), (mm[0], mm[1]))

    for cen in tb.CENARIOS:
        run = os.path.join(a.runs, "teste-%s" % cen)
        pcap = os.path.join(run, "scn.pcap")
        macs = tb.le_macs(os.path.join(run, "injector.mac"))
        for nome, (clf, bm, mm) in modelos.items():
            st = ref.FeatureStreamer()
            X, y, info = [], [], []
            for p, classe, _raw in tb.pacotes(pcap, macs, ref):
                X.append(st.step(p, bm))
                y.append(classe)
                info.append((p.src, "SD" if p.is_sd else ("svc 0x%04x" % p.service_id if p.service_id >= 0 else
                                                       ("TCP sem payload" if not p.pl_l4 else "outro"))))
            X = np.asarray(X, dtype=np.float64)
            y = np.asarray(y)
            pred = tb.prediz(clf, tb.normaliza(X, mm))
            fp = (y == 0) & (pred != 0)
            n0 = int((y == 0).sum())
            print("%-12s modelo %-8s FP %6.2f%% (%d de %d benignos)" % (cen, nome, 100.0 * fp.sum() / n0, int(fp.sum()), n0))
            c = Counter((info[i][0], info[i][1], tb.CLASSES[pred[i]]) for i in np.flatnonzero(fp))
            for (src, tipo, cl), k in c.most_common(5):
                print("      %6d  origem %-13s %-16s -> %s" % (k, src, tipo, cl))


if __name__ == "__main__":
    main()
