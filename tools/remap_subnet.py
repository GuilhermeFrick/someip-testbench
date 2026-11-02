#!/usr/bin/env python3
"""Deriva uma matriz igual à original, só com os endereços em outra sub-rede.

Por que isto existe, e por que não é editar a matriz na mão.

O `platform/comm_matrix_kim.yaml` **descreve um barramento que já existe**, e o
próprio arquivo avisa que editá-lo deixa de ser reprodução e passa a ser
variação. Mas a sub-rede dele, `172.18.0.0/24`, é a faixa que o Docker costuma
entregar ao segundo projeto de qualquer máquina — e numa máquina onde ela já está
tomada, não há como subir os nós naquele endereçamento.

Então a sub-rede vira um **desvio declarado**: esta ferramenta escreve uma matriz
derivada, com um cabeçalho dizendo de onde veio e o que mudou, e o original fica
intocado. O que a derivada preserva é o que importa para a reprodução:

* o **último octeto** de cada nó, que é o que distingue os nove entre si e o que
  aparece nos rótulos por endereço de origem;
* tudo o mais — serviços, instâncias, portas, eventos, cadências, tamanhos de
  payload, eventgroups e quem assina o quê.

O que muda é só o prefixo. Endereço não é feature do detector: ele é chave de
fluxo. Dois nós que antes eram `.9` e `.10` continuam sendo `.9` e `.10`, em outro
prefixo, e a contagem de fluxos não se altera.

⚠️ O multicast do Service Discovery **não** é remapeado. Ele não pertence à
sub-rede dos nós, e trocá-lo mudaria o endereço de destino das ofertas, que é
observável no barramento.

Uso:
    python tools/remap_subnet.py --matrix platform/comm_matrix_kim.yaml \\
                                 --subnet 172.28.0.0/24 \\
                                 --out build/run/comm_matrix_run.yaml
"""
from __future__ import annotations

import argparse
import os
import sys


def prefixo_de(subnet: str) -> str:
    """Os três primeiros octetos de uma sub-rede, com o ponto final.

    Aceita `172.28.0.0/24` e `172.28.0.0`. Recusa qualquer outra coisa, em vez de
    adivinhar: um prefixo errado aqui gera nove containers que não se enxergam, e
    o sintoma seria "o Service Discovery não funciona".
    """
    rede = subnet.split("/")[0]
    partes = rede.split(".")
    if len(partes) != 4:
        raise ValueError(f"sub-rede nao reconhecida: {subnet!r}")
    for p in partes:
        if (not p.isdigit()) or (not 0 <= int(p) <= 255):
            raise ValueError(f"sub-rede nao reconhecida: {subnet!r}")
    return ".".join(partes[:3]) + "."


def ultimo_octeto(ip: str) -> str:
    """O último octeto de um endereço, que é o que a derivada preserva."""
    partes = ip.strip().split(".")
    if len(partes) != 4:
        raise ValueError(f"endereco nao reconhecido: {ip!r}")
    return partes[3]


def remapeia(linhas: list, prefixo: str, origem: str, subnet: str) -> list:
    """Reescreve as linhas de `subnet:` e de `ip:` preservando todo o resto.

    É feito por LINHA, de propósito, e não carregando/reserializando o YAML: a
    matriz recuperada tem comentários que explicam o que a captura não carrega, e
    um round-trip por `yaml.safe_load` + `yaml.dump` os apagaria junto com a
    ordem dos campos. A derivada precisa poder ser comparada com `diff` à
    original, e a diferença precisa ser só o que mudou.
    """
    saida = [
        "# ⚠️ MATRIZ DERIVADA, GERADA. Nao edite: mexa no original e gere de novo.",
        f"#   origem:   {origem}",
        f"#   mudanca:  sub-rede dos nos -> {subnet}",
        "#",
        "# O ultimo octeto de cada no foi PRESERVADO, porque e ele que distingue os nos",
        "# entre si e e por ele que o rotulo por endereco de origem e atribuido. Servicos,",
        "# instancias, portas, eventos, cadencias, tamanhos de payload, eventgroups e",
        "# assinaturas sao os mesmos. O multicast do Service Discovery NAO foi mexido.",
        "#",
        "# Isto existe porque a sub-rede da matriz original pode ja estar em uso na maquina",
        "# onde a bancada sobe. E um desvio DECLARADO de endereçamento, nao de comportamento.",
        "",
    ]

    trocadas_ip = 0
    trocadas_rede = 0
    for linha in linhas:
        sem_comentario = linha.split("#", 1)[0]
        chave = sem_comentario.strip()

        if chave.startswith("subnet:"):
            recuo = linha[: len(linha) - len(linha.lstrip())]
            saida.append(f"{recuo}subnet: {subnet}")
            trocadas_rede += 1
        elif chave.startswith("ip:"):
            recuo = linha[: len(linha) - len(linha.lstrip())]
            antigo = chave.split(":", 1)[1].strip()
            saida.append(f"{recuo}ip: {prefixo}{ultimo_octeto(antigo)}")
            trocadas_ip += 1
        else:
            saida.append(linha.rstrip("\n"))

    if trocadas_rede == 0:
        raise ValueError("a matriz nao tem linha `subnet:`; nada foi remapeado")
    if trocadas_ip == 0:
        raise ValueError("a matriz nao tem linha `ip:` alguma; nada foi remapeado")

    print(f"  {trocadas_ip} enderecos de no e {trocadas_rede} sub-rede(s) -> {subnet}")
    return saida


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--matrix", required=True, help="matriz de origem")
    ap.add_argument("--subnet", required=True, help="sub-rede nova, p.ex. 172.28.0.0/24")
    ap.add_argument("--out", required=True, help="matriz derivada a escrever")
    a = ap.parse_args()

    with open(a.matrix, "r", encoding="utf-8") as fh:
        linhas = fh.read().splitlines()

    try:
        prefixo = prefixo_de(a.subnet)
        saida = remapeia(linhas, prefixo, os.path.relpath(a.matrix), a.subnet)
    except ValueError as erro:
        print(f"ERRO: {erro}", file=sys.stderr)
        raise SystemExit(1)

    destino = os.path.dirname(os.path.abspath(a.out))
    os.makedirs(destino, exist_ok=True)
    with open(a.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(saida) + "\n")
    print(f"[ok] {os.path.abspath(a.out)}")


if __name__ == "__main__":
    main()
