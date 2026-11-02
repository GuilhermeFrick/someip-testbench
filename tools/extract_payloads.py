#!/usr/bin/env python3
"""Extrai de uma captura os bytes de payload de cada evento, em ORDEM.

Para que serve, e por que é um diagnóstico e não um produto.

A medição de `experiments/eval-pcap/RESULTADOS.md` mostrou que o conteúdo do
payload é o termo dominante do falso positivo: as duas políticas sintéticas do nó
(`counter` e `random`) são ruins por motivos **opostos** — uma quase
determinística, outra uniforme. O que nenhuma das duas responde é *quanto do
buraco é só conteúdo*. Alimentar os nós com os bytes do próprio barramento de
referência responde: se o falso positivo cair ao nível do dataset, o conteúdo é a
lacuna inteira e todo o resto da bancada está certo.

⚠️ **Isto é circular como alegação de produto** e não deve ser apresentado como
"a bancada reproduz o barramento". É o oposto: é o **limite superior** do que a
reprodução pode alcançar, e serve para dizer se vale a pena ir atrás de conteúdo
realista (CARLA) ou se há outra diferença escondida.

⚠️ **A ORDEM É PRESERVADA, e isso não é detalhe.** Quatro das dezesseis features
olham para a relação entre um pacote e o anterior do mesmo fluxo
(`someip_change`, `sd_change`, `l4_change`, `repeat_rate`). Amostrar os payloads
aleatoriamente destruiria essa estrutura e produziria um terceiro tipo de tráfego
sintético, não o do dataset. Por isso se guarda um trecho **contíguo** de cada
fluxo, do começo da captura.

O parser é o de REFERÊNCIA (`--repro`), não uma reimplementação: extrair com uma
segunda implementação introduziria uma diferença que depois seria atribuída ao
domínio.

Formato de saída (`payloads.bin`), little-endian:

    magic      4 bytes  "SPAY"
    versao     u32      1
    nFluxos    u32
    por fluxo: servico u16, evento u16, tamanho u32, nAmostras u32,
               depois nAmostras * tamanho bytes

Cada fluxo tem tamanho FIXO porque é isso que o dataset tem: medido sobre o
benigno, os onze fluxos de notificação têm um único tamanho cada, e eles batem
com os `payload_bytes` que o `tools/extract_matrix.py` recuperou. Um fluxo com
tamanho variável é recusado aqui, em vez de virar artefato que o nó lê errado.

Uso:
    python3 tools/extract_payloads.py --repro <repo-de-reproducao> \\
                                      --pcap benign_traffic.pcap \\
                                      --out build/cfg/payloads.bin
"""
from __future__ import annotations

import argparse
import collections
import os
import struct
import sys

MAGIC = b"SPAY"
VERSAO = 1
MSG_NOTIFICATION = 0x02


def carrega_parser(repro: str):
    """Importa o parser de referência, que é a definição do que é uma mensagem."""
    src = os.path.join(os.path.expanduser(repro), "src")
    if not os.path.isdir(src):
        raise SystemExit(f"--repro nao tem src/: {src}")
    sys.path.insert(0, src)
    import someip as someip_mod  # noqa: E402
    return someip_mod


def coleta(pcap: str, someip_mod, limite: int):
    """Percorre a captura e guarda, por (serviço, evento), os payloads em ordem.

    Só NOTIFICAÇÃO (`msg_type` 0x02). Requisição e resposta existem no dataset mas
    são o vaivém de assinatura e valor inicial, não o fluxo cíclico que os nós da
    bancada publicam; misturá-los poria no fluxo de um evento bytes de outra coisa.
    """
    from scapy.utils import RawPcapReader

    fluxos = collections.OrderedDict()
    tamanhos = collections.defaultdict(collections.Counter)
    vistos = 0

    for raw, meta in RawPcapReader(pcap):
        p = someip_mod.parse(raw)
        if (p is None) or (not p.is_someip) or p.is_sd:
            continue
        if p.msg_type != MSG_NOTIFICATION:
            continue
        chave = (p.service_id, p.method_id)
        tamanhos[chave][len(p.pl_someip)] += 1
        vistos += 1
        destino = fluxos.setdefault(chave, [])
        if len(destino) < limite:
            destino.append(bytes(p.pl_someip))
        else:
            # Ja tem o bastante deste fluxo. A leitura CONTINUA mesmo assim: parar
            # cedo perderia um fluxo que so aparece depois, e o histograma de
            # tamanhos (que decide se o fluxo e de registro fixo) precisa da
            # captura inteira para ser uma afirmacao e nao uma amostra.
            pass

    return fluxos, tamanhos, vistos


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repro", required=True, help="raiz do repo de reproducao")
    ap.add_argument("--pcap", required=True, help="captura de onde tirar os payloads")
    ap.add_argument("--out", default="build/cfg/payloads.bin")
    ap.add_argument("--limite", type=int, default=20000,
                    help="amostras por fluxo (padrao 20000: uma rodada de 60 s a 5 ms usa 12 000)")
    a = ap.parse_args()

    someip_mod = carrega_parser(a.repro)
    pcap = os.path.expanduser(a.pcap)
    if not os.path.exists(pcap):
        raise SystemExit(f"PCAP ausente: {pcap}")

    print(f"lendo {os.path.basename(pcap)}...")
    fluxos, tamanhos, vistos = coleta(pcap, someip_mod, a.limite)
    if not fluxos:
        raise SystemExit("nenhuma notificacao SOME/IP na captura")

    print(f"{vistos} notificacoes, {len(fluxos)} fluxos\n")
    print("%-8s %-8s %8s %10s  %s" % ("servico", "evento", "tamanho", "amostras", "observacao"))

    blocos = []
    for chave in fluxos:
        servico, evento = chave
        distintos = tamanhos[chave]
        if len(distintos) != 1:
            # Recusar e melhor do que gravar um fluxo que o no leria com o tamanho
            # errado: o formato e de registro fixo, e o tamanho vem da matriz.
            raise SystemExit("fluxo 0x%04X/0x%04X tem tamanhos variados: %s"
                             % (servico, evento, dict(distintos)))
        tamanho = next(iter(distintos))
        amostras = fluxos[chave]
        obs = ""
        if len(amostras) < a.limite:
            obs = "a captura so tinha isto"
        print("0x%04X   0x%04X   %8d %10d  %s" % (servico, evento, tamanho, len(amostras), obs))
        blocos.append((servico, evento, tamanho, amostras))

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "wb") as fh:
        fh.write(MAGIC)
        fh.write(struct.pack("<II", VERSAO, len(blocos)))
        for servico, evento, tamanho, amostras in blocos:
            fh.write(struct.pack("<HHII", servico, evento, tamanho, len(amostras)))
            for payload in amostras:
                fh.write(payload)

    print("\n%d bytes -> %s" % (os.path.getsize(a.out), a.out))
    print("ORDEM PRESERVADA: trecho contiguo do inicio da captura, por fluxo.")
    print("Quatro das 16 features olham para o pacote ANTERIOR do fluxo; amostrar")
    print("aleatoriamente produziria um terceiro trafego sintetico, nao o do dataset.")


if __name__ == "__main__":
    main()
