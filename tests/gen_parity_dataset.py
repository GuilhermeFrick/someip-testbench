#!/usr/bin/env python3
"""Gera a referência de paridade do extrator sobre o DATASET INTEIRO.

Diferença em relação a `gen_parity_vectors.py`: lá os pacotes são sintéticos e
escolhidos por mim, para cobrir os casos de borda que eu soube imaginar. Aqui os
pacotes são os 14 milhões do dataset real, e o que o teste pega é exatamente o
que eu NÃO soube imaginar.

O lado Python é o código que treinou o modelo: `src/someip.py`,
`src/bytemodel.py` e o `FeatureStreamer` de `src/extract_ext.py` do repositório
de reprodução, importados, não reimplementados.

Saída, em `--out`:

    dataset_bytemodels.bin   as três tabelas (L, 256) em float64, no formato que
                             o `ids::ByteModels::Create` lê; treinadas no PCAP
                             benigno, como em `extract.fit_bytemodels`
    manifest.txt             uma linha por PCAP: contagens e nome do arquivo de
                             registros
    <stem>.f32               um registro por VETOR emitido: índice do pacote em
                             uint32 e as 16 features em float32, ambos
                             little-endian

Por que float32 e não float64: o lado C++ produz `float`, então é nele que a
comparação pode ser bit a bit. É o mesmo contrato do teste de paridade
sintético. Uma divergência menor que a resolução do float32 não é detectada por
esta comparação; a que importa, de ordenação de operações ou de janela, é.

Por que o índice do pacote vai em cada registro: se um lado descartar um pacote
que o outro aceitou, as contagens divergem e tudo depois disso parece errado. Com
o índice, o teste aponta o pacote exato.

Uso, a partir da raiz do someip-testbench:
    python3 tests/gen_parity_dataset.py --repro ../someip-ids-multiclass-contentext \\
                                        --pcap-dir ~/dataset/data/pcap \\
                                        --out build/parity-dataset
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import time

RECORD_STRUCT = struct.Struct("<I16f")


def dump_models(models, L, path):
    """Escreve as tabelas no formato que o `ids::ByteModels::Create` espera.

    Mesma rotina de `gen_parity_vectors.py`: ordem someip, sd, l4, cada tabela
    precedida pelo número de posições.
    """
    with open(path, "wb") as fh:
        for nome in ("someip", "sd", "l4"):
            m = models[nome]
            posicoes = L[nome]
            if m.logP.shape != (posicoes, 256):
                raise SystemExit("tabela %s com forma %s, esperado (%d, 256)" % (nome, m.logP.shape, posicoes))
            fh.write(struct.pack("<I", posicoes))
            for i in range(posicoes):
                fh.write(struct.pack("<256d", *m.logP[i].tolist()))
    return os.path.getsize(path)


def process_file(path, models, ext_mod, someip_mod, ts_of, out_path, max_packets=None):
    """Roda o FeatureStreamer de referência sobre um PCAP e grava os registros.

    Um streamer NOVO por arquivo, porque é o que `extract_file` faz: o estado de
    janela não atravessa a fronteira entre cenários.
    """
    from scapy.utils import RawPcapReader

    streamer = ext_mod.FeatureStreamer()
    pacotes = 0
    descartados = 0
    linhas = 0
    inicio = time.time()
    with open(out_path, "wb") as fh:
        for raw, meta in RawPcapReader(path):
            pacotes += 1
            if max_packets and pacotes > max_packets:
                pacotes -= 1
                break
            p = someip_mod.parse(raw, ts_of(meta))
            if p is None:
                descartados += 1
                continue
            f = streamer.step(p, models)
            fh.write(RECORD_STRUCT.pack(pacotes, *(float(v) for v in f)))
            linhas += 1
    return {
        "pacotes": pacotes,
        "descartados": descartados,
        "linhas": linhas,
        "fluxos": len(streamer.last),
        "origens": len(streamer.src_pld),
        "segundos": time.time() - inicio,
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repro", required=True, help="raiz do repo someip-ids-multiclass-contentext")
    ap.add_argument("--pcap-dir", required=True, help="diretorio com os 7 PCAPs do dataset")
    ap.add_argument("--out", default="build/parity-dataset")
    ap.add_argument("--max-packets", type=int, default=None,
                    help="limite por arquivo, para um ensaio rapido antes da rodada inteira")
    a = ap.parse_args()

    sys.path.insert(0, os.path.join(a.repro, "src"))
    import extract as extract_mod       # noqa: E402
    import extract_ext as ext_mod       # noqa: E402
    import someip as someip_mod         # noqa: E402

    pcap_dir = os.path.expanduser(a.pcap_dir)
    os.makedirs(a.out, exist_ok=True)

    # A ordem é a de `extract.run`: o benigno primeiro, depois os cenários de
    # ataque. Ela não afeta a paridade, porque cada arquivo tem o seu streamer,
    # mas mantém o relatório comparável com o do repositório de reprodução.
    arquivos = [extract_mod.BENIGN_FILE] + list(extract_mod.ATTACK_FILES.keys())
    faltando = [f for f in arquivos if not os.path.exists(os.path.join(pcap_dir, f))]
    if faltando:
        raise SystemExit("PCAP ausente em %s: %s" % (pcap_dir, ", ".join(faltando)))

    print("[1/2] modelos de bytes no benigno (L=%s)..." % (extract_mod.L,))
    inicio = time.time()
    models, benign_ips = extract_mod.fit_bytemodels(pcap_dir, alpha=1.0, max_packets=a.max_packets)
    bin_path = os.path.join(a.out, "dataset_bytemodels.bin")
    tamanho = dump_models(models, extract_mod.L, bin_path)
    print("      %d IPs benignos, %d bytes -> %s (%.1f s)"
          % (len(benign_ips), tamanho, bin_path, time.time() - inicio))

    print("[2/2] features de referencia, arquivo por arquivo...")
    manifest = os.path.join(a.out, "manifest.txt")
    total_pacotes = 0
    total_linhas = 0
    with open(manifest, "w", encoding="utf-8", newline="\n") as mf:
        mf.write("# gerado por tests/gen_parity_dataset.py\n")
        mf.write("# referencia = src/someip.py + src/extract_ext.py do repo de reproducao\n")
        mf.write("# registros: uint32 indice_do_pacote + 16 float32, little-endian, %d bytes cada\n"
                 % RECORD_STRUCT.size)
        mf.write("# campos: <pcap> <registros> <pacotes> <descartados> <linhas> <fluxos> <origens>\n")
        for fname in arquivos:
            caminho = os.path.join(pcap_dir, fname)
            stem = os.path.splitext(os.path.basename(fname))[0]
            rec_name = stem + ".f32"
            r = process_file(caminho, models, ext_mod, someip_mod, extract_mod._ts,
                             os.path.join(a.out, rec_name), a.max_packets)
            mf.write("%s %s %d %d %d %d %d\n"
                     % (fname, rec_name, r["pacotes"], r["descartados"], r["linhas"],
                        r["fluxos"], r["origens"]))
            mf.flush()
            total_pacotes += r["pacotes"]
            total_linhas += r["linhas"]
            print("      %-42s %9d pacotes, %9d linhas, %6d fluxos, %4d origens, %6.1f s"
                  % (fname, r["pacotes"], r["linhas"], r["fluxos"], r["origens"], r["segundos"]))

    print()
    print("total: %d pacotes, %d vetores de referencia" % (total_pacotes, total_linhas))
    print("manifest -> %s" % manifest)


if __name__ == "__main__":
    main()
