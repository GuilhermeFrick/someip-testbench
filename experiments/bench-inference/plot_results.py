#!/usr/bin/env python3
"""Plota os resultados do benchmark de inferencia.

Le results.json (gerado pelo bench.c) e python_timing.json (gerado pelo prepare.py)
e escreve as figuras em figs/, nos temas claro e escuro.

Uso:  python plot_results.py
"""
from __future__ import annotations

import json
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
FIGS = os.path.join(HERE, "figs")

# --- paletas validadas com scripts/validate_palette.js ---
# categorica: slots 1-3 (azul, laranja, aqua). Cor segue a ENTIDADE (o motor)
# em todos os graficos, para o leitor rastrear o mesmo motor entre figuras.
THEMES = {
    "light": dict(
        surface="#fcfcfb", ink="#0b0b0b", ink2="#52514e", grid="#e3e2dd",
        series=("#2a78d6", "#eb6834", "#1baf7a"),
    ),
    "dark": dict(
        surface="#1a1a19", ink="#f0efec", ink2="#c3c2b7", grid="#383835",
        series=("#3987e5", "#d95926", "#199e70"),
    ),
}

ENGINES = ["libxgboost", "TL2cgen", "avaliador\ncompacto"]
KEYS = ["libxgboost_c_api", "tl2cgen", "avaliador_compacto"]


def load():
    r = json.load(open(os.path.join(HERE, "results.json"), encoding="utf-8"))
    p = json.load(open(os.path.join(HERE, "python_timing.json"), encoding="utf-8"))
    return r, p


def style(ax, t, xlabel=None):
    ax.set_facecolor(t["surface"])
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(t["grid"])
    ax.tick_params(colors=t["ink2"], length=0)
    ax.xaxis.grid(True, color=t["grid"], lw=0.8)
    ax.set_axisbelow(True)
    if xlabel:
        ax.set_xlabel(xlabel, color=t["ink2"], fontsize=10)


def save(fig, name, t, theme):
    os.makedirs(FIGS, exist_ok=True)
    out = os.path.join(FIGS, f"{name}-{theme}.png")
    fig.savefig(out, dpi=200, bbox_inches="tight", facecolor=t["surface"])
    plt.close(fig)
    print("  ->", os.path.relpath(out, HERE))


# ---------------------------------------------------------------- figura 1
def fig_latencia(r, p, t, theme):
    """Magnitude: mediana por motor, com Python e o piso em lote como referencias."""
    med = [r["latency_us"][k]["p50"] for k in KEYS]
    fig, ax = plt.subplots(figsize=(7.4, 3.5))
    y = range(len(ENGINES))
    ax.barh(y, med, height=0.62, color=t["series"], zorder=3)

    for i, v in enumerate(med):
        ax.text(v + 14, i, f"{v:.1f} µs".replace(".", ","), va="center",
                color=t["ink"], fontsize=11, fontweight="bold", zorder=4)

    # linhas de referencia: rotuladas no TOPO da area do grafico, para nao
    # colidirem com o rotulo do eixo x
    floor = p["python_batch_us"]
    pysingle = p["python_single_us"]
    for x, lab, ha in ((floor, f"piso em lote  {floor:.1f} µs".replace(".", ","), "left"),
                       (pysingle, f"Python, 1 linha  {pysingle:.0f} µs (média)  ", "right")):
        ax.axvline(x, color=t["ink2"], ls="--", lw=1.2, zorder=2)
        ax.annotate(("  " if ha == "left" else "") + lab, xy=(x, 1.0),
                    xycoords=("data", "axes fraction"), xytext=(0, -4),
                    textcoords="offset points", color=t["ink2"], fontsize=8.5,
                    va="top", ha=ha)

    ax.set_yticks(list(y))
    ax.set_yticklabels(ENGINES, color=t["ink"], fontsize=10)
    ax.invert_yaxis()
    ax.set_ylim(len(ENGINES) - 0.5, -1.05)   # folga no topo p/ os rotulos de referencia
    ax.set_xlim(0, pysingle * 1.08)
    style(ax, t, "latência por predição, 1 linha por chamada (µs) — menor é melhor")
    ax.set_title("Inferência de 1 pacote: mediana por motor",
                 color=t["ink"], fontsize=12.5, pad=12, loc="left")
    save(fig, "fig1-latencia", t, theme)


# ---------------------------------------------------------------- figura 2
def fig_cauda(r, t, theme):
    """Cauda: a media esconde o p99, que e o que importa num IDS."""
    fig, ax = plt.subplots(figsize=(7.4, 3.3))
    for i, (k, c) in enumerate(zip(KEYS, t["series"])):
        s = r["latency_us"][k]
        ax.plot([s["p50"], s["p99"]], [i, i], color=c, lw=2.5, zorder=3,
                solid_capstyle="round")
        ax.plot([s["p50"]], [i], "o", color=c, ms=9, zorder=4)
        ax.plot([s["p99"]], [i], "|", color=c, ms=14, mew=2.5, zorder=4)
        ax.text(s["p50"], i - 0.3, f"p50 {s['p50']:.0f}", color=t["ink"],
                fontsize=9, ha="center", va="bottom")
        ax.text(s["p99"], i + 0.3, f"p99 {s['p99']:.0f}", color=t["ink2"],
                fontsize=9, ha="center", va="top")

    ax.set_yticks(range(len(ENGINES)))
    ax.set_yticklabels(ENGINES, color=t["ink"], fontsize=10)
    ax.invert_yaxis()
    ax.set_ylim(len(ENGINES) - 0.4, -0.6)
    ax.margins(x=0.06)
    style(ax, t, "latência (µs) — do p50 ao p99")
    ax.set_title("Cauda da latência: a ordem dos motores não muda no p99",
                 color=t["ink"], fontsize=12.5, pad=12, loc="left")
    save(fig, "fig2-cauda", t, theme)


# ---------------------------------------------------------------- figura 3
def fig_footprint(r, t, theme):
    """A causa: o TL2cgen vira CODIGO; o avaliador compacto vira DADO."""
    nodes = r["setup"]["n_nodes"]
    tab_mb = nodes * 16 / 1e6
    vals = [11.7, tab_mb]
    labs = ["TL2cgen\n(objeto compilado = código)",
            "avaliador compacto\n(tabela de nós = dado)"]
    cols = [t["series"][1], t["series"][2]]

    fig, ax = plt.subplots(figsize=(7.4, 2.6))
    ax.barh(range(2), vals, height=0.55, color=cols, zorder=3)
    for i, v in enumerate(vals):
        ax.text(v + 0.25, i, f"{v:.2f} MB".replace(".", ","),
                va="center", color=t["ink"], fontsize=11, fontweight="bold")

    ax.set_yticks(range(2))
    ax.set_yticklabels(labs, color=t["ink"], fontsize=10)
    ax.invert_yaxis()
    ax.set_xlim(0, 13.8)
    style(ax, t, "tamanho do artefato de inferência (MB)")
    ax.set_title("Por que o TL2cgen perde: 4,5x mais bytes, e são de código",
                 color=t["ink"], fontsize=12.5, pad=12, loc="left")
    save(fig, "fig3-footprint", t, theme)


def main():
    r, p = load()
    for theme, t in THEMES.items():
        print(f"tema {theme}:")
        fig_latencia(r, p, t, theme)
        fig_cauda(r, t, theme)
        fig_footprint(r, t, theme)


if __name__ == "__main__":
    main()
