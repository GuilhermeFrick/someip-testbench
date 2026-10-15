#!/usr/bin/env python3
"""Prepara o head-to-head libxgboost x TL2cgen.

Faz tres coisas:
 1. Exporta o modelo XGBoost para C99 com TL2cgen (e mostra a assinatura gerada).
 2. Gera as linhas de entrada e as probabilidades de referencia (oraculo Python).
 3. Mede a latencia do lado Python (1 linha por chamada e em lote), para contexto.

Uso:
    python prepare.py --model /caminho/model_16.json --out build
"""
from __future__ import annotations

import argparse
import json
import os
import time

import numpy as np

N_ROWS = 512          # linhas usadas no benchmark e na paridade
N_FEAT = 16
SEED = 20261015


def export_tl2cgen(model_path: str, outdir: str) -> bool:
    """Gera C99 a partir do modelo. Devolve True se conseguiu."""
    try:
        import treelite
        import tl2cgen
    except ImportError:
        print("[tl2cgen] NAO instalado. Rode: pip install treelite tl2cgen")
        return False

    # o nome do loader mudou entre versoes do treelite
    loader = None
    for name in ("load_xgboost_model", "load_xgboost_model_legacy_binary"):
        loader = getattr(treelite.frontend, name, None)
        if loader is not None:
            break
    if loader is None:
        loader = getattr(treelite, "Model", None)
        if loader is not None:
            loader = treelite.Model.load  # fallback antigo
    if loader is None:
        print("[tl2cgen] nao achei o loader de modelo XGBoost no treelite")
        return False

    print(f"[tl2cgen] treelite={treelite.__version__} tl2cgen={tl2cgen.__version__}")
    model = loader(model_path)
    cdir = os.path.join(outdir, "tl2cgen_c")
    os.makedirs(cdir, exist_ok=True)
    tl2cgen.generate_c_code(model, dirpath=cdir, params={})

    files = sorted(os.listdir(cdir))
    print(f"[tl2cgen] gerado em {cdir}: {files}")

    # mostra a assinatura: e isso que o bench.c precisa casar
    hdr = os.path.join(cdir, "header.h")
    if os.path.exists(hdr):
        print("\n--- header.h gerado (assinatura a casar no bench.c) ---")
        with open(hdr, encoding="utf-8", errors="replace") as fh:
            print(fh.read())
        print("--- fim do header ---\n")
    else:
        for f in files:
            if f.endswith(".h"):
                print(f"\n--- {f} ---")
                with open(os.path.join(cdir, f), encoding="utf-8", errors="replace") as fh:
                    print(fh.read())
    return True


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True, help="model_16.json salvo pelo XGBoost")
    ap.add_argument("--out", default="build")
    ap.add_argument("--n-rows", type=int, default=N_ROWS)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    import xgboost as xgb
    print(f"[xgboost] versao {xgb.__version__}")

    bst = xgb.Booster()
    bst.load_model(a.model)
    bst.set_param({"nthread": "1"})

    # Entradas: as features vao normalizadas em [0,1] no pipeline, entao uniforme serve.
    # O que importa e que Python e C recebam EXATAMENTE as mesmas linhas.
    rng = np.random.default_rng(SEED)
    X = rng.random((a.n_rows, N_FEAT), dtype=np.float32)

    # Oraculo: probabilidades do proprio XGBoost
    ref = bst.inplace_predict(X)
    ref = np.asarray(ref, dtype=np.float32)
    n_class = ref.shape[1] if ref.ndim > 1 else 1
    print(f"[oraculo] shape={ref.shape} n_class={n_class}")

    X.tofile(os.path.join(a.out, "rows.bin"))
    ref.tofile(os.path.join(a.out, "ref_probs.bin"))
    json.dump({"n_rows": int(a.n_rows), "n_feat": N_FEAT, "n_class": int(n_class)},
              open(os.path.join(a.out, "meta.json"), "w"), indent=2)

    # --- latencia do lado Python, para contexto ---
    def timeit(fn, n):
        fn()                                  # aquecimento
        t0 = time.perf_counter()
        for _ in range(n):
            fn()
        return (time.perf_counter() - t0) / n * 1e6   # us por chamada

    one = X[:1]
    us_single = timeit(lambda: bst.inplace_predict(one), 2000)
    us_batch = timeit(lambda: bst.inplace_predict(X), 200) / a.n_rows

    print("\n=== LATENCIA PYTHON (contexto) ===")
    print(f"  inplace_predict 1 linha por chamada : {us_single:9.1f} us/pred")
    print(f"  inplace_predict em lote ({a.n_rows} linhas): {us_batch:9.2f} us/pred")
    print(f"  razao (custo fixo por chamada)      : {us_single / max(us_batch, 1e-9):9.0f}x")

    json.dump({"python_single_us": us_single, "python_batch_us": us_batch},
              open(os.path.join(a.out, "python_timing.json"), "w"), indent=2)

    print()
    export_tl2cgen(a.model, a.out)
    print(f"\n[ok] artefatos em {os.path.abspath(a.out)}")


if __name__ == "__main__":
    main()
