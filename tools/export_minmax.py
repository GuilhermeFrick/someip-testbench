#!/usr/bin/env python3
"""Exporta os limites min-max do treino para o formato que o `ids::Normalizer` lê.

Formato, little-endian: `uint32` com o número de features, depois essa
quantidade de `double` de xmin, depois a mesma quantidade de xmax.

Três fontes, e elas NÃO são equivalentes:

  --from-npy <minmax.npy>     O artefato que o `pipeline.run` do repositório de
                              reprodução salva. Use quando existir.

  --from-pcap <dir>           RECALCULA os limites a partir dos PCAPs, do mesmo
                              jeito que o `extract_all()` do `pipeline.py` os
                              calcula: ajusta os modelos de bytes no benigno,
                              roda o `FeatureStreamer` de referência sobre os 7
                              arquivos e acumula mínimo e máximo por coluna em
                              **float64**. Dá os limites EXATOS, sem precisar do
                              `minmax.npy`. Leva uns 20 minutos.

  --from-records <dir>        Atalho, a partir dos registros `.f32` da rodada de
                              paridade. Os registros guardam as features em
                              float32, então os limites saem arredondados.

Por que `--from-pcap` dá o resultado exato e não "um resultado parecido":
mínimo e máximo por coluna não dependem da ordem em que os valores aparecem,
então acumular em fluxo dá o mesmo que o `Xraw.min(0)` do pipeline, que materia-
liza a matriz inteira. E o `fit_bytemodels` é determinístico dados o PCAP
benigno e o alpha, então os modelos de bytes também são os mesmos.

Sobre o atalho `--from-records`, com precisão: o arredondamento float64 para
float32 é monótono, então o mínimo dos valores arredondados é exatamente o
arredondamento do mínimo. Os limites obtidos dali são, portanto,
`round_f32(xmin)` e `round_f32(xmax)` dos verdadeiros — não uma aproximação de
qualidade desconhecida, e sim o arredondamento deles.

⚠️ Em qualquer caso, o `Normalizer` exige que os três artefatos (modelo, modelos
de bytes e limites) venham da MESMA rodada. Recalcular os limites é legítimo
porque o cálculo é determinístico e parte dos mesmos PCAPs; inventar limites
novos, não.

Uso:
    python3 tools/export_minmax.py --from-npy results/minmax.npy --out build/cfg/minmax.bin
    python3 tools/export_minmax.py --from-pcap ~/dataset/data/pcap \\
            --repro ../someip-ids-multiclass-contentext --out build/cfg/minmax.bin
    python3 tools/export_minmax.py --from-records build/parity-dataset --out build/cfg/minmax.bin
"""
from __future__ import annotations

import argparse
import glob
import os
import struct
import sys

N_FEAT = 16
RECORD = struct.Struct("<I16f")


def de_npy(caminho: str):
    """Lê o minmax.npy do pipeline de treino: duas linhas, xmin e xmax."""
    import numpy as np

    m = np.load(caminho)
    if m.shape != (2, N_FEAT):
        raise SystemExit("minmax.npy com forma %s, esperado (2, %d)" % (m.shape, N_FEAT))
    return [float(v) for v in m[0]], [float(v) for v in m[1]], "treino (float64)"


def de_registros(diretorio: str):
    """Varre os registros .f32 da paridade e acumula o mínimo e o máximo por feature.

    A ordem dos arquivos não importa para mínimo e máximo, mas é a do manifesto
    para que o relatório saia comparável com o da rodada.
    """
    arquivos = sorted(glob.glob(os.path.join(diretorio, "*.f32")))
    if not arquivos:
        raise SystemExit("nenhum .f32 em %s. Rode antes a paridade do dataset." % diretorio)

    xmin = [float("inf")] * N_FEAT
    xmax = [float("-inf")] * N_FEAT
    total = 0
    for caminho in arquivos:
        with open(caminho, "rb") as fh:
            while True:
                # Lê em blocos: um registro por vez em 14 milhões de registros
                # seria uma chamada de sistema por registro.
                bloco = fh.read(RECORD.size * 8192)
                if not bloco:
                    break
                for (_, *vals) in RECORD.iter_unpack(bloco):
                    total += 1
                    for i in range(N_FEAT):
                        v = vals[i]
                        if v < xmin[i]:
                            xmin[i] = v
                        if v > xmax[i]:
                            xmax[i] = v
        print("      %-44s acumulado" % os.path.basename(caminho))
    print("      %d vetores lidos" % total)
    return xmin, xmax, "registros da paridade (float32 arredondado)"


def de_pcap(pcap_dir: str, repro: str):
    """Recalcula os limites em float64, do mesmo jeito que o `extract_all()` do pipeline.

    Mínimo e máximo por coluna não dependem da ordem, então acumular em fluxo dá
    exatamente o mesmo que materializar a matriz e chamar `Xraw.min(0)`. A
    diferença é a memória: 14,2 M por 16 em float64 são 1,8 GB.
    """
    import sys as _sys
    import time

    from scapy.utils import RawPcapReader

    _sys.path.insert(0, os.path.join(repro, "src"))
    import extract as extract_mod       # noqa: E402
    import extract_ext as ext_mod       # noqa: E402
    import someip as someip_mod         # noqa: E402

    pcap_dir = os.path.expanduser(pcap_dir)
    arquivos = [extract_mod.BENIGN_FILE] + list(extract_mod.ATTACK_FILES.keys())
    faltando = [f for f in arquivos if not os.path.exists(os.path.join(pcap_dir, f))]
    if faltando:
        raise SystemExit("PCAP ausente em %s: %s" % (pcap_dir, ", ".join(faltando)))

    print("[1/2] modelos de bytes no benigno (deterministico, alpha=1.0)...")
    t0 = time.time()
    models, _ = extract_mod.fit_bytemodels(pcap_dir, alpha=1.0, max_packets=None)
    print("      %.1f s" % (time.time() - t0))

    print("[2/2] acumulando minimo e maximo em float64, arquivo por arquivo...")
    xmin = [float("inf")] * N_FEAT
    xmax = [float("-inf")] * N_FEAT
    total = 0
    for fname in arquivos:
        t0 = time.time()
        # Um streamer NOVO por arquivo, porque e o que o extract_file faz.
        streamer = ext_mod.FeatureStreamer()
        n = 0
        for raw, meta in RawPcapReader(os.path.join(pcap_dir, fname)):
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
        print("      %-44s %9d vetores, %6.1f s" % (fname, n, time.time() - t0))
    print("      %d vetores no total" % total)
    return xmin, xmax, "recalculado dos PCAPs (float64, exato)"


def main() -> None:
    ap = argparse.ArgumentParser()
    fonte = ap.add_mutually_exclusive_group(required=True)
    fonte.add_argument("--from-npy", help="minmax.npy salvo pelo pipeline de treino")
    fonte.add_argument("--from-pcap", help="diretorio com os 7 PCAPs; recalcula em float64")
    fonte.add_argument("--from-records", help="diretorio com os .f32 da paridade")
    ap.add_argument("--repro", help="raiz do repo de reproducao (exigido por --from-pcap)")
    ap.add_argument("--out", default="build/cfg/minmax.bin")
    ap.add_argument("--names", action="store_true", help="imprime os nomes das features no relatorio")
    a = ap.parse_args()

    if a.from_npy:
        xmin, xmax, origem = de_npy(a.from_npy)
    elif a.from_pcap:
        if not a.repro:
            raise SystemExit("--from-pcap exige --repro: os modulos de referencia vem de la")
        xmin, xmax, origem = de_pcap(a.from_pcap, a.repro)
    else:
        xmin, xmax, origem = de_registros(a.from_records)

    nomes = ["f%d" % i for i in range(N_FEAT)]
    if a.names:
        nomes = [
            "ip_time_interval", "someip_loglik", "sd_loglik", "l4_loglik",
            "someip_entropy", "sd_entropy", "l4_entropy",
            "someip_change", "sd_change", "l4_change", "ip_len_change", "l4_len_change",
            "repeat_rate", "someip_len", "l4_len", "src_payload_div",
        ]

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "wb") as fh:
        fh.write(struct.pack("<I", N_FEAT))
        fh.write(struct.pack("<%dd" % N_FEAT, *xmin))
        fh.write(struct.pack("<%dd" % N_FEAT, *xmax))

    print()
    print("fonte: %s" % origem)
    print("%-20s %18s %18s %s" % ("feature", "xmin", "xmax", ""))
    degeneradas = []
    for i in range(N_FEAT):
        aviso = ""
        if not (xmax[i] > xmin[i]):
            aviso = "<- DEGENERADA: divisor 1,0"
            degeneradas.append(nomes[i])
        print("%-20s %18.9g %18.9g %s" % (nomes[i], xmin[i], xmax[i], aviso))

    print()
    if degeneradas:
        # Nao e erro: a referencia divide por 1,0 nesse caso, e o Normalizer faz
        # igual. Mas e informacao que vale aparecer, porque uma feature constante
        # no treino nao carrega informacao nenhuma para as arvores.
        print("ATENCAO: %d feature(s) constante(s) no treino: %s" % (len(degeneradas), ", ".join(degeneradas)))
        print("         A referencia divide por 1,0 nesse caso, e o Normalizer reproduz isso.")
    else:
        print("nenhuma feature degenerada: todo divisor e xmax-xmin")
    print("%d bytes -> %s" % (os.path.getsize(a.out), a.out))


if __name__ == "__main__":
    main()
    sys.exit(0)
