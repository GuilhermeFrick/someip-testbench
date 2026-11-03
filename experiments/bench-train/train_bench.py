#!/usr/bin/env python3
"""Treina a receita da C1 com dados da bancada e avalia em 2 x 2 (modelo x domínio).

Pergunta: o falso positivo alto da bancada é limitação do MODELO da C1 (16 features
`content_ext`, modelos de bytes, XGBoost) ou do DADO com que ele foi treinado (o
barramento do Kim, com o carro quase sempre parado)? A receita é a MESMA, importada do
repositório de referência (`--repro`): modelos de bytes ajustados no tráfego benigno,
`FeatureStreamer`, normalização min-max, `model_params()`. Só mudam os dados e o rótulo,
que aqui é o MAC de quem pôs o quadro no fio (o IP pode ser forjado).

Modelos:
  * kim     — os artefatos da C1 (`--kim-art`: model_16.json, bytemodels.npz, minmax.npy);
  * bancada — treinado aqui nas rodadas de treino (`runs/treino-*`).

Conjuntos de teste:
  * bancada   — rodadas de teste (`runs/teste-*`), independentes das de treino;
  * kim       — os últimos 30% de cada PCAP do dataset (o teste temporal da C1);
  * novidade  — rodadas só benignas com conteúdo que nenhum dos dois viu (`--novelty`).

Saídas em `--out`: métricas (metrics.json), e os artefatos do modelo da bancada no
formato da C1, para `tools/build_sensor_artefacts.sh` levá-los ao sensor em C++.

Uso (na imagem someip-parity:1, que tem xgboost, scapy e numpy):
    python3 experiments/bench-train/train_bench.py --repro /work/someip-ids-multiclass-contentext \\
        --runs experiments/bench-train/runs --kim-art /work/artefatos --kim-pcap /d \\
        --novelty platform/scenario-live/out-none-sintetico --out experiments/bench-train/out
"""
import argparse
import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sd_features import SD_NOMES, SdStreamer  # noqa: E402

CLASSES = ["normal", "dos", "fuzzy", "mitm_single", "mitm_multi"]
CLASSE_DO_PAPEL = {"dos": 1, "fuzzy": 2, "mitm_single": 3, "mitm_relay": 4, "mitm_impostor": 4}
CENARIOS = ["none", "dos", "fuzzy", "mitm_single", "mitm_multi"]
# PCAPs do dataset do Kim e o MAC de cada atacante (tools/sd_detail.py).
KIM = [("benign_traffic.pcap", {}),
       ("dos_noti_flood.pcap", {"02:42:ac:12:00:0b": 1}),
       ("fuzzy_sd_offer_rand_noti(1).pcap", {"02:42:ac:12:00:11": 2}),
       ("mitm_single_attacker.pcap", {"02:42:ac:12:00:0d": 3}),
       ("mitm_multi_attacker.pcap", {"02:42:ac:12:00:0e": 4, "02:42:ac:12:00:0f": 4})]


def le_macs(path):
    macs = {}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as fh:
            for linha in fh:
                p = linha.split()
                if len(p) == 2:
                    macs[p[0].lower()] = CLASSE_DO_PAPEL[p[1]]
    return macs


def pacotes(pcap, macs, ref):
    """(pacote parseado, classe pelo MAC, quadro cru), na ordem da captura. Só o que o parser aceita."""
    from scapy.utils import RawPcapReader
    for raw, meta in RawPcapReader(pcap):
        p = ref.someip.parse(raw, ref._ts(meta))
        if p is None:
            continue
        yield p, macs.get(bytes(raw[6:12]).hex(":"), 0), raw


def ajusta_bytemodels(fontes, ref):
    """Modelos de bytes no tráfego BENIGNO das fontes, como fit_bytemodels() da C1."""
    models = {k: ref.ByteModel(ref.L[k], 1.0) for k in ref.L}
    for pcap, macs in fontes:
        for p, classe, _raw in pacotes(pcap, macs, ref):
            if classe != 0:
                continue
            if p.pl_l4:
                models["l4"].update(p.pl_l4)
            if p.is_sd and p.pl_sd:
                models["sd"].update(p.pl_sd)
            elif p.pl_someip:
                models["someip"].update(p.pl_someip)
    for m in models.values():
        m.finalize()
    return models


def carrega_bytemodels(npz, ref):
    d = np.load(npz)
    models = {}
    for k in ref.L:
        m = ref.ByteModel(ref.L[k], 1.0)
        m.logP = d[k]
        models[k] = m
    return models


def extrai(pcap, macs, models, ref, so_fim=None):
    """Features cruas e rótulos de um PCAP. `so_fim` = fração final a manter (0.3 no Kim).

    Com `ref.sd` ligado, acrescenta as 5 features de SD (sd_features.py) depois das 16."""
    st = ref.FeatureStreamer()
    sds = SdStreamer() if getattr(ref, "sd", False) else None
    X, y = [], []
    for p, classe, raw in pacotes(pcap, macs, ref):
        f = tuple(st.step(p, models))
        if sds is not None:
            f = f + sds.step(raw, p.src, p.dst, p.service_id, p.ts)
        X.append(f)
        y.append(classe)
    X = np.asarray(X, dtype=np.float64)
    y = np.asarray(y, dtype=np.int64)
    if so_fim:
        corte = int(len(y) * (1.0 - so_fim))
        X, y = X[corte:], y[corte:]
    return X, y


def normaliza(X, mm):
    xmin, xmax = mm
    rng = np.where(xmax > xmin, xmax - xmin, 1.0)
    return ((X - xmin) / rng).astype(np.float32)


def pesos_por_classe(y, potencia=0.5):
    """Peso de cada amostra pelo inverso da frequência da sua classe, elevado a `potencia`.

    A C1 não ponderava. Na bancada o Fuzzy tem 435 pacotes contra 880 mil normais, e com
    tão poucos exemplos acrescentar uma feature muda como as árvores separam a classe: o
    modelo cria regiões espúrias de Fuzzy que capturam tráfego benigno (ablate_sd.py).
    potencia=0.5 (raiz) amortece o peso para não inverter o problema.
    """
    cont = np.bincount(y, minlength=len(CLASSES)).astype(np.float64)
    w = np.where(cont > 0, (cont.sum() / (len(CLASSES) * np.maximum(cont, 1.0))) ** potencia, 0.0)
    return w[y]


def treina(Xn, y, params, pesos=None):
    """XGBoost pela API nativa, com os hiperparâmetros de pipeline.model_params().

    A imagem da paridade não tem scikit-learn, que o XGBClassifier exige. A tradução
    é direta: n_estimators vira o número de rodadas, learning_rate vira eta, n_jobs
    vira nthread; o resto passa igual.
    """
    import xgboost as xgb
    p = dict(params)
    rodadas = p.pop("n_estimators")
    p["eta"] = p.pop("learning_rate")
    p["nthread"] = p.pop("n_jobs")
    return xgb.train(p, xgb.DMatrix(Xn, label=y, weight=pesos), num_boost_round=rodadas)


def prediz(booster, Xn):
    import xgboost as xgb
    return booster.predict(xgb.DMatrix(Xn)).argmax(1)


def avalia(clf, Xn, y):
    pred = prediz(clf, Xn)
    k = len(CLASSES)
    cm = np.bincount(y * k + pred, minlength=k * k).reshape(k, k)
    res = {"quadros": int(len(y)), "por_classe": {}}
    for i, c in enumerate(CLASSES):
        tp = int(cm[i, i])
        sup = int(cm[i].sum())
        pp = int(cm[:, i].sum())
        if sup == 0 and pp == 0:
            continue
        res["por_classe"][c] = {"suporte": sup,
                                "recall": tp / sup if sup else None,
                                "precisao": tp / pp if pp else None}
    nor = cm[0].sum()
    res["falso_positivo"] = float((nor - cm[0, 0]) / nor) if nor else None
    res["confusao"] = cm.tolist()
    return res


def imprime(nome, r):
    fp = r["falso_positivo"]
    print("  %-28s %8d quadros   FP %s" % (nome, r["quadros"], "-" if fp is None else "%.3f%%" % (100 * fp)))
    for c, v in r["por_classe"].items():
        if c == "normal":
            continue
        rec = "-" if v["recall"] is None else "%.3f" % v["recall"]
        pre = "-" if v["precisao"] is None else "%.3f" % v["precisao"]
        print("      %-12s suporte %7d  recall %s  precisão %s" % (c, v["suporte"], rec, pre))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repro", required=True)
    ap.add_argument("--runs", required=True)
    ap.add_argument("--kim-art", required=True)
    ap.add_argument("--kim-pcap", required=True)
    ap.add_argument("--novelty", action="append", default=[], help="rodada só benigna com conteúdo novo")
    ap.add_argument("--out", required=True)
    ap.add_argument("--n-jobs", type=int, default=4)
    ap.add_argument("--sd", action="store_true", help="acrescenta as 5 features de SD (sd_features.py)")
    ap.add_argument("--pesos", type=float, default=None,
                    help="pesos por classe, inverso da frequência elevado a este expoente (0.5 = raiz)")
    ap.add_argument("--features", default="",
                    help="features do modelo da bancada, por nome e separadas por vírgula; vazio = as 16")
    a = ap.parse_args()

    sys.path.insert(0, os.path.join(a.repro, "src"))
    import types
    import someip
    import extract
    import extract_ext
    import bytemodel
    import pipeline
    import xgboost as xgb
    ref = types.SimpleNamespace(someip=someip, _ts=extract._ts, L=extract.L, ByteModel=bytemodel.ByteModel,
                                FeatureStreamer=extract_ext.FeatureStreamer, sd=a.sd)
    os.makedirs(a.out, exist_ok=True)
    t0 = time.time()
    nomes = list(extract_ext.FEATURE_NAMES) + (SD_NOMES if a.sd else [])
    cols = list(range(len(nomes))) if not a.features else [nomes.index(n.strip()) for n in a.features.split(",")]
    print("[0] modelo da bancada com %d feature(s): %s" % (len(cols), ", ".join(nomes[i] for i in cols)))

    treino = [(os.path.join(a.runs, "treino-%s" % s, "scn.pcap"), le_macs(os.path.join(a.runs, "treino-%s" % s, "injector.mac")))
              for s in CENARIOS]
    teste = [(s, os.path.join(a.runs, "teste-%s" % s, "scn.pcap"), le_macs(os.path.join(a.runs, "teste-%s" % s, "injector.mac")))
             for s in CENARIOS]

    # ---- modelo da bancada: a receita da C1 sobre as rodadas de treino ----
    print("[1] modelos de bytes no benigno das rodadas de treino")
    bm_banc = ajusta_bytemodels(treino, ref)
    print("[2] features das rodadas de treino")
    partes = [extrai(p, m, bm_banc, ref) for p, m in treino]
    Xtr = np.concatenate([x for x, _ in partes])
    ytr = np.concatenate([y for _, y in partes])
    Xtr = Xtr[:, cols]
    mm_banc = (Xtr.min(0), Xtr.max(0))
    print("    %d quadros de treino: %s" % (len(ytr), dict(zip(CLASSES, np.bincount(ytr, minlength=5).tolist()))))
    clf_banc = treina(normaliza(Xtr, mm_banc), ytr, pipeline.model_params(n_jobs=a.n_jobs),
                      pesos_por_classe(ytr, a.pesos) if a.pesos is not None else None)
    clf_banc.save_model(os.path.join(a.out, "model_16.json"))
    np.savez_compressed(os.path.join(a.out, "bytemodels.npz"),
                        someip=bm_banc["someip"].logP, sd=bm_banc["sd"].logP, l4=bm_banc["l4"].logP)
    np.save(os.path.join(a.out, "minmax.npy"), np.vstack(mm_banc))

    # ---- modelo do Kim: os artefatos da C1 ----
    bm_kim = carrega_bytemodels(os.path.join(a.kim_art, "bytemodels.npz"), ref)
    mmk = np.load(os.path.join(a.kim_art, "minmax.npy"))
    mm_kim = (mmk[0], mmk[1])
    clf_kim = xgb.Booster(model_file=os.path.join(a.kim_art, "model_16.json"))

    modelos = {"kim": (clf_kim, bm_kim, mm_kim, list(range(16))), "bancada": (clf_banc, bm_banc, mm_banc, cols)}
    resultados = {}

    def junta(itens, modelo, so_fim=None):
        clf, bm, mm, cs = modelos[modelo]
        partes = [extrai(p, m, bm, ref, so_fim) for p, m in itens]
        X = np.concatenate([x for x, _ in partes])[:, cs]
        y = np.concatenate([yy for _, yy in partes])
        return avalia(clf, normaliza(X, mm), y)

    print("[3] avaliação")
    for modelo in modelos:
        resultados[modelo] = {}
        resultados[modelo]["bancada"] = junta([(p, m) for _, p, m in teste], modelo)
        resultados[modelo]["kim"] = junta([(os.path.join(a.kim_pcap, f), m) for f, m in KIM], modelo, so_fim=0.3)
        for nv in a.novelty:
            nome = "novidade:" + os.path.basename(nv.rstrip("/"))
            resultados[modelo][nome] = junta([(os.path.join(nv, "scn.pcap"), {})], modelo)

    resultados["features_da_bancada"] = [nomes[i] for i in cols]
    json.dump(resultados, open(os.path.join(a.out, "metrics.json"), "w"), indent=2)
    print()
    for modelo, por in resultados.items():
        if not isinstance(por, dict):
            continue
        print("MODELO %s" % modelo)
        for conj, r in por.items():
            imprime("teste " + conj, r)
    print("\n[ok] %.0f s; artefatos e métricas em %s" % (time.time() - t0, a.out))


if __name__ == "__main__":
    main()
