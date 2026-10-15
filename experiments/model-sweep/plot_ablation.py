#!/usr/bin/env python3
"""Mapa de calor da ablacao: F1 por classe para cada subconjunto de features.

Forma escolhida: a grandeza e UMA so (F1) sobre duas dimensoes categoricas
(subconjunto x classe), entao mapa de calor com rampa sequencial de um matiz.
Nao e identidade, e magnitude: cor categorica aqui estaria errada.
"""
import json, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LinearSegmentedColormap

HERE = os.path.dirname(os.path.abspath(__file__))
CLS = ["normal", "dos", "fuzzy", "mitm_single", "mitm_multi"]
ORDER = ["all16", "no_lowgain", "no_srcdiv", "base12_rep", "base12", "lean"]
LABEL = {"all16": "all16  (16 feat)", "no_lowgain": "sem time/length  (13)",
         "no_srcdiv": "sem source diversity  (15)", "base12_rep": "base12 + repeticao  (13)",
         "base12": "base12  (12)", "lean": "lean  (12)"}
# rampa sequencial azul, passos 100..700 da paleta validada
RAMP = ["#cde2fb", "#9ec5f4", "#6da7ec", "#3987e5", "#2a78d6", "#1c5cab", "#0d366b"]
THEMES = {"light": dict(surface="#fcfcfb", ink="#0b0b0b", ink2="#52514e"),
          "dark": dict(surface="#1a1a19", ink="#f0efec", ink2="#c3c2b7")}

rows = {(r["model"], r["subset"]): r for r in json.load(open(os.path.join(HERE, "ablation.json")))}
cmap = LinearSegmentedColormap.from_list("seq", RAMP)

for theme, t in THEMES.items():
    data = [[rows[("r50_d4", s)]["f1"][c] for c in CLS] + [rows[("r50_d4", s)]["macro_f1"]]
            for s in ORDER]
    fig, ax = plt.subplots(figsize=(8.2, 3.6))
    im = ax.imshow(data, cmap=cmap, vmin=0.3, vmax=1.0, aspect="auto")
    cols = [c.replace("_", "-") for c in CLS] + ["macro-F1"]
    ax.set_xticks(range(len(cols))); ax.set_xticklabels(cols, color=t["ink"], fontsize=9.5)
    ax.set_yticks(range(len(ORDER)))
    ax.set_yticklabels([LABEL[s] for s in ORDER], color=t["ink"], fontsize=9.5)
    for i in range(len(ORDER)):
        for j in range(len(cols)):
            v = data[i][j]
            ax.text(j, i, f"{v:.3f}".replace(".", ","), ha="center", va="center",
                    fontsize=9, color="white" if v > 0.72 else "#0b0b0b")
    for s in ax.spines.values():
        s.set_visible(False)
    ax.tick_params(length=0)
    ax.set_title("Ablacao de features: remover a source diversity derruba Fuzzy E MITM-multi",
                 color=t["ink"], fontsize=12, pad=12, loc="left")
    fig.colorbar(im, fraction=0.025, pad=0.02).ax.tick_params(colors=t["ink2"], length=0)
    out = os.path.join(HERE, "figs", f"fig-ablacao-{theme}.png")
    fig.savefig(out, dpi=200, bbox_inches="tight", facecolor=t["surface"])
    plt.close(fig); print(" ->", os.path.basename(out))
