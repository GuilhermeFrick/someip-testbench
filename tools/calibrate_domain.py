#!/usr/bin/env python3
"""Calibra as duas camadas NÃO SUPERVISIONADAS do sensor num barramento novo.

Esta é a ferramenta que a segunda contribuição promete e que não existia. O
detector tem três camadas, e elas precisam de coisas diferentes:

| camada | o que é | precisa de |
|---|---|---|
| as 16 features | código | nada, é fixo |
| modelos de bytes + limites min/max | estatística do barramento | **tráfego benigno, sem rótulo** |
| classificador de árvores | fronteira entre as classes | rótulo, treino de frota |

O meio é o que esta ferramenta produz. A promessa de implantabilidade é exatamente
essa: **o classificador fica congelado** e cada carro calibra as duas camadas
estatísticas no próprio tráfego, sem rótulo nenhum. Um `fit` de contagem de bytes
e um `min/max` por coluna não exigem saber o que é ataque — exigem só uma janela
de tráfego normal.

⚠️ **A janela TEM de ser benigna.** Não há como a ferramenta verificar isso: ela
não tem rótulo, e é esse o ponto. Ataque dentro da janela de calibração entra na
estatística como se fosse normal e alarga a faixa justamente onde a fronteira
importa. Quem opera garante a janela; a ferramenta registra no relatório quantos
pacotes e quais origens entraram, para a garantia ficar auditável.

Os módulos de referência vêm do repositório de reprodução (`--repro`), **não** de
uma reimplementação: calibrar com uma segunda implementação das features
introduziria uma diferença que depois seria atribuída ao domínio.

Uso:
    python3 tools/calibrate_domain.py --repro ~/work/someip-ids-multiclass-contentext \\
                                      --pcap captura.pcap \\
                                      --out build/recal

Saída, no formato que o `ids_sensor` carrega:
    bytemodels.bin   as três tabelas (L, 256) ajustadas NESTE barramento
    minmax.bin       os limites das 16 features NESTE barramento
    relatorio.txt    o que entrou na calibração, para auditoria
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import time

N_FEAT = 16


def carrega_referencia(repro: str):
    """Importa os módulos de referência, que são a definição das features."""
    src = os.path.join(os.path.expanduser(repro), "src")
    if not os.path.isdir(src):
        raise SystemExit(f"--repro nao tem src/: {src}")
    sys.path.insert(0, src)
    import bytemodel as bytemodel_mod   # noqa: E402
    import extract as extract_mod       # noqa: E402
    import extract_ext as ext_mod       # noqa: E402
    import someip as someip_mod         # noqa: E402
    return bytemodel_mod, extract_mod, ext_mod, someip_mod


def ajusta_bytemodels(pcaps: list, extract_mod, bytemodel_mod, someip_mod):
    """Ajusta os três modelos de bytes sobre os PCAPs dados.

    É o `fit_bytemodels()` da referência com uma diferença só, e ela é a razão
    desta ferramenta existir: a função original lê `BENIGN_FILE`, um nome de
    arquivo FIXO do dataset. Num barramento novo o arquivo tem outro nome, e o
    conjunto pode ser mais de um. A acumulação por byte e posição não depende da
    ordem, então vários arquivos somam exatamente como um só.
    """
    from scapy.utils import RawPcapReader

    L = extract_mod.L
    models = {k: bytemodel_mod.ByteModel(L[k], 1.0) for k in L}
    origens = set()
    lidos = 0
    aceitos = 0

    for caminho in pcaps:
        t0 = time.time()
        n = 0
        for raw, meta in RawPcapReader(caminho):
            lidos += 1
            p = someip_mod.parse(raw)
            if p is None:
                continue
            n += 1
            origens.add(p.src)
            if p.pl_l4:
                models["l4"].update(p.pl_l4)
            if p.is_sd and p.pl_sd:
                models["sd"].update(p.pl_sd)
            elif p.pl_someip:
                models["someip"].update(p.pl_someip)
        aceitos += n
        print("      %-40s %8d aceitos, %5.1f s" % (os.path.basename(caminho), n, time.time() - t0))

    for m in models.values():
        m.finalize()
    return models, origens, lidos, aceitos


def acumula_limites(pcaps: list, models, extract_mod, ext_mod, someip_mod):
    """Mínimo e máximo por feature, em float64, com os modelos recém-ajustados.

    Um streamer NOVO por arquivo, porque é o que o `extract_file()` da referência
    faz: as janelas de repetição e de diversidade não atravessam arquivo.
    """
    from scapy.utils import RawPcapReader

    xmin = [float("inf")] * N_FEAT
    xmax = [float("-inf")] * N_FEAT
    total = 0
    for caminho in pcaps:
        t0 = time.time()
        streamer = ext_mod.FeatureStreamer()
        n = 0
        for raw, meta in RawPcapReader(caminho):
            p = someip_mod.parse(raw, extract_mod._ts(meta))
            if p is None:
                continue
            f = streamer.step(p, models)
            for i in range(N_FEAT):
                v = float(f[i])
                if v < xmin[i]:
                    xmin[i] = v
                if v > xmax[i]:
                    xmax[i] = v
            n += 1
        total += n
        print("      %-40s %8d vetores, %5.1f s" % (os.path.basename(caminho), n, time.time() - t0))
    return xmin, xmax, total


def escreve_minmax(xmin: list, xmax: list, destino: str):
    """Escreve no formato que o `Normalizer::Create` lê.

    Reusa o escritor do `tools/export_minmax.py`, para o formato existir num lugar
    só: dois formatos de limites divergindo em silêncio seria o mesmo defeito que
    os três artefatos de rodadas diferentes.
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import export_minmax  # noqa: E402

    export_minmax.escreve(xmin, xmax, destino)


def escreve_bytemodels(models, destino: str, extract_mod):
    """Escreve as três tabelas no formato que o `ByteModels::Create` lê.

    O formato vem do `tools/export_bytemodels.py`, que hoje só sabe ler um `.npz`
    salvo pelo treino. Aqui os modelos já estão em memória, então o que se reusa é
    a função de escrita.
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import export_bytemodels  # noqa: E402

    tabelas = {k: models[k].logP for k in ("someip", "sd", "l4")}
    export_bytemodels.escreve(tabelas, destino)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repro", required=True, help="raiz do repo de reproducao; de la vem a definicao das features")
    ap.add_argument("--pcap", action="append", required=True, help="PCAP de trafego BENIGNO deste barramento; pode repetir")
    ap.add_argument("--out", default="build/recal", help="diretorio de saida")
    a = ap.parse_args()

    pcaps = [os.path.expanduser(p) for p in a.pcap]
    for p in pcaps:
        if not os.path.exists(p):
            raise SystemExit(f"PCAP ausente: {p}")

    bytemodel_mod, extract_mod, ext_mod, someip_mod = carrega_referencia(a.repro)
    os.makedirs(a.out, exist_ok=True)

    print("[1/2] ajustando os modelos de bytes neste barramento (alpha=1.0)...")
    models, origens, lidos, aceitos = ajusta_bytemodels(pcaps, extract_mod, bytemodel_mod, someip_mod)

    print("[2/2] acumulando minimo e maximo em float64, com os modelos novos...")
    xmin, xmax, total = acumula_limites(pcaps, models, extract_mod, ext_mod, someip_mod)

    escreve_bytemodels(models, os.path.join(a.out, "bytemodels.bin"), extract_mod)
    escreve_minmax(xmin, xmax, os.path.join(a.out, "minmax.bin"))

    # O relatorio existe porque a garantia de "a janela era benigna" e humana: ele
    # registra o que entrou, para a garantia poder ser conferida depois.
    linhas = [
        "calibracao de dominio",
        "  arquivos:        %s" % ", ".join(os.path.basename(p) for p in pcaps),
        "  quadros lidos:   %d" % lidos,
        "  aceitos:         %d" % aceitos,
        "  vetores:         %d" % total,
        "  origens vistas:  %s" % ", ".join(sorted(origens)),
        "",
        "AVISO: a ferramenta NAO verifica que a janela e benigna -- nao ha rotulo, e e esse o",
        "ponto. Ataque dentro da janela entra na estatistica como normal e alarga a faixa",
        "exatamente onde a fronteira importa.",
    ]
    with open(os.path.join(a.out, "relatorio.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(linhas) + "\n")

    print()
    print("\n".join(linhas))
    print()
    print(f"[ok] {os.path.abspath(a.out)}")


if __name__ == "__main__":
    main()
