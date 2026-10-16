#!/usr/bin/env python3
"""Gráficos da seleção de hiperparâmetros.

Duas figuras, com propósitos distintos:

  fig-selecao-validacao  a varredura como ela foi feita: macro-F1 de VALIDAÇÃO
                         por configuração, com a vencedora destacada. É a
                         evidência do critério.

  fig-validacao-vs-teste o argumento do método: cada configuração plotada por
                         validação (x) contra teste (y). Mostra que a
                         configuração com o melhor TESTE tinha validação
                         medíocre, ou seja, vantagem no conjunto de reporte.

Lê `selecao.json` (validação) e `sweep_full.json` (teste das mesmas 15).
"""
from __future__ import annotations

import json
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
FIGS = os.path.join(HERE, "figs")

# paleta validada; cor segue a ENTIDADE (profundidade) nas duas figuras
THEMES = {
    "light": dict(surface="#fcfcfb", ink="#0b0b0b", ink2="#52514e", grid="#e3e2dd",
                  series={4: "#2a78d6", 6: "#eb6834", 8: "#1baf7a"}),
    "dark": dict(surface="#1a1a19", ink="#f0efec", ink2="#c3c2b7", grid="#383835",
                 series={4: "#3987e5", 6: "#d95926", 8: "#199e70"}),
}
ROUNDS = [25, 50, 100, 200, 300]


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


def legend(ax, t):
    leg = ax.legend(fontsize=9, facecolor=t["surface"], edgecolor=t["grid"])
    for txt in leg.get_texts():
        txt.set_color(t["ink"])


def load():
    sel = json.load(open(os.path.join(HERE, "selecao.json"), encoding="utf-8"))
    full = json.load(open(os.path.join(HERE, "sweep_full.json"), encoding="utf-8"))
    test = {(r["rounds"], r["depth"]): r for r in full}
    rows = []
    for v in sel["validacao"]:
        k = (v["rounds"], v["depth"])
        rows.append({**v, "macro_f1_test": test[k]["macro_f1"],
                     "n_nodes": test[k]["n_nodes"]})
    return rows, sel


def fig_validacao(rows, sel, t, theme):
    """A varredura como foi feita: so validacao, que e o criterio."""
    best = max(rows, key=lambda r: r["macro_f1_val"])
    fig, ax = plt.subplots(figsize=(7.4, 4.3))
    for d in (4, 6, 8):
        pts = sorted([r for r in rows if r["depth"] == d], key=lambda r: r["rounds"])
        ax.plot([r["rounds"] for r in pts], [r["macro_f1_val"] for r in pts],
                "-o", color=t["series"][d], lw=1.7, ms=6,
                label=f"profundidade {d}", zorder=3)
    ax.plot([best["rounds"]], [best["macro_f1_val"]], "o", ms=15, mfc="none",
            mec=t["ink"], mew=2, zorder=4)
    # a virgula decimal vale so para o numero, senao "prof." vira "prof,"
    ax.annotate(f"escolhido\n{best['rounds']} rodadas, prof. {best['depth']}\n"
                f"macro-F1 {best['macro_f1_val']:.4f}".replace("0.", "0,"),
                xy=(best["rounds"], best["macro_f1_val"]), xytext=(60, 0.905),
                color=t["ink"], fontsize=9,
                arrowprops=dict(arrowstyle="->", color=t["ink2"], lw=1.2))
    ax.set_xscale("log"); ax.set_xticks(ROUNDS)
    ax.get_xaxis().set_major_formatter(matplotlib.ticker.ScalarFormatter())
    style(ax, t, "rodadas de boosting (escala log)", "macro-F1 na VALIDAÇÃO")
    ax.set_title("Varredura: o critério é a validação, nunca o teste",
                 color=t["ink"], fontsize=12.5, pad=12, loc="left")
    legend(ax, t)
    out = os.path.join(FIGS, f"fig-selecao-validacao-{theme}.png")
    fig.savefig(out, dpi=200, bbox_inches="tight", facecolor=t["surface"])
    plt.close(fig); print("  ->", os.path.basename(out))


def fig_val_vs_test(rows, t, theme):
    """O argumento do metodo: melhor teste nao implica melhor validacao."""
    best_val = max(rows, key=lambda r: r["macro_f1_val"])
    best_test = max(rows, key=lambda r: r["macro_f1_test"])
    fig, ax = plt.subplots(figsize=(7.6, 4.8))

    # Zoom na regiao estavel. As 3 configuracoes de profundidade 6 que colapsam
    # (macro-F1 de teste ~0,57) ficam fora do eixo de proposito: incluidas, elas
    # comprimem a regiao que interessa ate ela virar ilegivel. O colapso e uma
    # anomalia a parte, documentada no RESULTADOS.md.
    XLIM, YLIM = (0.928, 0.966), (0.960, 0.975)
    fora = 0
    for d in (4, 6, 8):
        pts = [r for r in rows if r["depth"] == d]
        vis = [r for r in pts if XLIM[0] <= r["macro_f1_val"] <= XLIM[1]
               and YLIM[0] <= r["macro_f1_test"] <= YLIM[1]]
        fora += len(pts) - len(vis)
        ax.scatter([r["macro_f1_val"] for r in vis], [r["macro_f1_test"] for r in vis],
                   s=80, color=t["series"][d], label=f"profundidade {d}", zorder=3)

    ax.annotate(f"escolhido pela validação\nr{best_val['rounds']}_d{best_val['depth']}",
                xy=(best_val["macro_f1_val"], best_val["macro_f1_test"]),
                xytext=(best_val["macro_f1_val"] - 0.004, YLIM[0] + 0.0035),
                color=t["ink"], fontsize=9, ha="right",
                arrowprops=dict(arrowstyle="->", color=t["ink2"], lw=1.2))
    ax.annotate(f"melhor no TESTE, 7º na validação\nr{best_test['rounds']}_d{best_test['depth']}",
                xy=(best_test["macro_f1_val"], best_test["macro_f1_test"]),
                xytext=(best_test["macro_f1_val"] + 0.004, YLIM[1] - 0.0025),
                color=t["ink"], fontsize=9, ha="left",
                arrowprops=dict(arrowstyle="->", color=t["ink2"], lw=1.2))

    ax.set_xlim(*XLIM); ax.set_ylim(*YLIM)
    style(ax, t, "macro-F1 na VALIDAÇÃO (critério de escolha)",
          "macro-F1 no TESTE (o que se reporta)")
    if fora:
        ax.text(0.5, -0.20, f"{fora} configurações de profundidade 6 ficam fora do eixo "
                            f"(colapso documentado no RESULTADOS.md)",
                transform=ax.transAxes, ha="center", color=t["ink2"], fontsize=8.5)
    ax.set_title("Melhor no teste não é melhor na validação",
                 color=t["ink"], fontsize=12.5, pad=12, loc="left")
    legend(ax, t)
    out = os.path.join(FIGS, f"fig-validacao-vs-teste-{theme}.png")
    fig.savefig(out, dpi=200, bbox_inches="tight", facecolor=t["surface"])
    plt.close(fig); print("  ->", os.path.basename(out))


def main():
    rows, sel = load()
    os.makedirs(FIGS, exist_ok=True)
    for theme, t in THEMES.items():
        print(f"tema {theme}:")
        fig_validacao(rows, sel, t, theme)
        fig_val_vs_test(rows, t, theme)


if __name__ == "__main__":
    main()
