#!/usr/bin/env python3
"""Plota a fronteira qualidade x custo da varredura de encolhimento.

Le sweep_full.json (metricas + latencia medida) e escreve figs/.
"""
from __future__ import annotations

import json
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
FIGS = os.path.join(HERE, "figs")

# mesma paleta validada do outro experimento; cor segue a ENTIDADE (profundidade)
THEMES = {
    "light": dict(surface="#fcfcfb", ink="#0b0b0b", ink2="#52514e", grid="#e3e2dd",
                  series={4: "#2a78d6", 6: "#eb6834", 8: "#1baf7a"}),
    "dark": dict(surface="#1a1a19", ink="#f0efec", ink2="#c3c2b7", grid="#383835",
                 series={4: "#3987e5", 6: "#d95926", 8: "#199e70"}),
}

ATUAL = "r300_d8"      # o modelo do artigo
MELHOR = "r50_d4"      # o vencedor da varredura


def style(ax, t, xlabel, ylabel):
    ax.set_facecolor(t["surface"])
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(t["grid"])
    ax.tick_params(colors=t["ink2"], length=0)
    ax.grid(True, color=t["grid"], lw=0.8)
    ax.set_axisbelow(True)
    ax.set_xlabel(xlabel, color=t["ink2"], fontsize=10)
    ax.set_ylabel(ylabel, color=t["ink2"], fontsize=10)


def main() -> None:
    rows = json.load(open(os.path.join(HERE, "sweep_full.json"), encoding="utf-8"))
    os.makedirs(FIGS, exist_ok=True)

    for theme, t in THEMES.items():
        fig, ax = plt.subplots(figsize=(7.6, 4.6))

        for depth in (4, 6, 8):
            pts = sorted([r for r in rows if r["depth"] == depth],
                         key=lambda r: r["p50_us"])
            ax.plot([r["p50_us"] for r in pts], [r["macro_f1"] for r in pts],
                    "-o", color=t["series"][depth], lw=1.6, ms=6,
                    label=f"profundidade {depth}", zorder=3)

        # destaca os dois pontos que importam
        for name, dy, lab in ((ATUAL, -0.018, "modelo do artigo"),
                              (MELHOR, 0.012, "melhor da varredura")):
            r = next(x for x in rows if x["name"] == name)
            ax.annotate(f"{lab}\n{name}: {r['macro_f1']:.4f} em {r['p50_us']:.2f} µs"
                        .replace(".", ","),
                        xy=(r["p50_us"], r["macro_f1"]),
                        xytext=(r["p50_us"] + 4, r["macro_f1"] + dy),
                        color=t["ink"], fontsize=9,
                        arrowprops=dict(arrowstyle="-", color=t["ink2"], lw=1))

        style(ax, t, "latência mediana por predição (µs) — menor é melhor",
              "macro-F1 — maior é melhor")
        ax.set_title("Fronteira qualidade × custo: o modelo do artigo está fora dela",
                     color=t["ink"], fontsize=12.5, pad=12, loc="left")
        leg = ax.legend(fontsize=9, facecolor=t["surface"], edgecolor=t["grid"])
        for txt in leg.get_texts():
            txt.set_color(t["ink"])
        ax.set_ylim(0.54, 1.0)
        ax.set_xlim(-2, 78)

        out = os.path.join(FIGS, f"fig-fronteira-{theme}.png")
        fig.savefig(out, dpi=200, bbox_inches="tight", facecolor=t["surface"])
        plt.close(fig)
        print("  ->", os.path.relpath(out, HERE))


if __name__ == "__main__":
    main()
