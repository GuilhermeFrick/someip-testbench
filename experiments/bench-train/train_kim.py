#!/usr/bin/env python3
"""Checagem simétrica: treina no dataset do Kim e testa na bancada.

train_bench.py mostrou que o modelo treinado na BANCADA só com as 5 features livres
de conteúdo transfere para o Kim. Aqui o sentido é o inverso, com o mesmo split da C1
(primeiros 70% de cada PCAP do Kim para treino, últimos 30% para teste) e o rótulo
pelo MAC de quem pôs o pacote no fio, que corrige os 163 188 StopOffer forjados do
MITM multi que a regra por IP do dataset rotula como normais.

Dois modelos, a mesma receita da C1:
  * c1_mac — as 16 features, rótulo por MAC: a C1 com o rótulo corrigido;
  * sem_conteudo — só as 5 features livres de conteúdo.

Os modelos de bytes são ajustados no PCAP benigno inteiro, como fez a C1
(fit_bytemodels); as 5 features não os usam.

Uso (na imagem someip-parity:1):
    python3 experiments/bench-train/train_kim.py --repro /work/someip-ids-multiclass-contentext \\
        --runs experiments/bench-train/runs --kim-pcap /d \\
        --novelty platform/scenario-live/out-none-sintetico --out experiments/bench-train/out-kim
"""
import argparse
import json
import os
import sys
import time
import types

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import train_bench as tb  # noqa: E402

SEM_CONTEUDO = ["ip_time_interval", "ip_len_change", "l4_len_change", "someip_len", "l4_len"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repro", required=True)
    ap.add_argument("--runs", required=True)
    ap.add_argument("--kim-pcap", required=True)
    ap.add_argument("--novelty", action="append", default=[])
    ap.add_argument("--out", required=True)
    ap.add_argument("--n-jobs", type=int, default=4)
    ap.add_argument("--sd", action="store_true", help="treina também 5 sem conteúdo + 5 de SD")
    ap.add_argument("--pesos", type=float, default=None,
                    help="pesos por classe, inverso da frequência elevado a este expoente (0.5 = raiz)")
    a = ap.parse_args()

    sys.path.insert(0, os.path.join(a.repro, "src"))
    import someip
    import extract
    import extract_ext
    import bytemodel
    import pipeline
    ref = types.SimpleNamespace(someip=someip, _ts=extract._ts, L=extract.L, ByteModel=bytemodel.ByteModel,
                                FeatureStreamer=extract_ext.FeatureStreamer, sd=a.sd)
    nomes = list(extract_ext.FEATURE_NAMES) + (tb.SD_NOMES if a.sd else [])
    os.makedirs(a.out, exist_ok=True)
    t0 = time.time()

    kim = [(os.path.join(a.kim_pcap, f), m) for f, m in tb.KIM]
    print("[1] modelos de bytes no PCAP benigno do Kim (inteiro, como a C1)")
    bm = tb.ajusta_bytemodels(kim[:1], ref)

    print("[2] features do Kim, split temporal 70/30 por arquivo")
    Xtr, ytr, Xte, yte = [], [], [], []
    for pcap, macs in kim:
        X, y = tb.extrai(pcap, macs, bm, ref)
        corte = int(len(y) * 0.7)
        Xtr.append(X[:corte]); ytr.append(y[:corte]); Xte.append(X[corte:]); yte.append(y[corte:])
        print("    %-36s %8d pacotes" % (os.path.basename(pcap), len(y)))
    Xtr = np.concatenate(Xtr); ytr = np.concatenate(ytr); Xte = np.concatenate(Xte); yte = np.concatenate(yte)
    print("    treino: %s" % dict(zip(tb.CLASSES, np.bincount(ytr, minlength=5).tolist())))

    print("[3] features da bancada e da novidade (mesmos modelos de bytes)")
    partes = [tb.extrai(os.path.join(a.runs, "teste-%s" % s, "scn.pcap"),
                        tb.le_macs(os.path.join(a.runs, "teste-%s" % s, "injector.mac")), bm, ref) for s in tb.CENARIOS]
    Xb = np.concatenate([x for x, _ in partes]); yb = np.concatenate([y for _, y in partes])
    novid = {}
    for nv in a.novelty:
        novid["novidade:" + os.path.basename(nv.rstrip("/"))] = tb.extrai(os.path.join(nv, "scn.pcap"), {}, bm, ref)

    resultados = {}
    variantes = [("c1_mac", list(range(16))), ("sem_conteudo", [nomes.index(n) for n in SEM_CONTEUDO])]
    if a.sd:
        variantes.append(("sem_conteudo_mais_sd", [nomes.index(n) for n in SEM_CONTEUDO + tb.SD_NOMES]))
    for nome, cols in variantes:
        mm = (Xtr[:, cols].min(0), Xtr[:, cols].max(0))
        clf = tb.treina(tb.normaliza(Xtr[:, cols], mm), ytr, pipeline.model_params(n_jobs=a.n_jobs),
                        tb.pesos_por_classe(ytr, a.pesos) if a.pesos is not None else None)
        d = os.path.join(a.out, nome)
        os.makedirs(d, exist_ok=True)
        clf.save_model(os.path.join(d, "model_16.json"))
        np.save(os.path.join(d, "minmax.npy"), np.vstack(mm))
        r = {"features": [nomes[i] for i in cols],
             "kim": tb.avalia(clf, tb.normaliza(Xte[:, cols], mm), yte),
             "bancada": tb.avalia(clf, tb.normaliza(Xb[:, cols], mm), yb)}
        for k, (X, y) in novid.items():
            r[k] = tb.avalia(clf, tb.normaliza(X[:, cols], mm), y)
        resultados[nome] = r
    np.savez_compressed(os.path.join(a.out, "bytemodels.npz"),
                        someip=bm["someip"].logP, sd=bm["sd"].logP, l4=bm["l4"].logP)
    json.dump(resultados, open(os.path.join(a.out, "metrics.json"), "w"), indent=2)

    print()
    for nome, por in resultados.items():
        print("MODELO treinado no Kim: %s (%d features)" % (nome, len(por["features"])))
        for conj, r in por.items():
            if conj != "features":
                tb.imprime("teste " + conj, r)
    print("\n[ok] %.0f s; métricas em %s" % (time.time() - t0, a.out))


if __name__ == "__main__":
    main()
