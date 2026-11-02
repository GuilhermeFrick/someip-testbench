#!/usr/bin/env python3
"""Emite a matriz de comunicação em **vCDL**, o formato do Vector CANoe.

Por que isto existe.

A segunda contribuição afirma que as cadeias de referência são proprietárias e
que isso limita a reprodutibilidade. O contraponto forte não é ignorar a
ferramenta comercial — é ser **interoperável com ela sem depender dela**. Com
este emissor, a mesma matriz que sobe os nós em vsomeip pode ser carregada no
CANoe por quem tiver licença. Um barramento, duas implementações.

E isso abre a validação mais forte que a bancada pode ter: montar a mesma matriz
nos dois, exportar PCAP de cada um e comparar. Hoje não dá (a demo online não
deixa arquivo sair), mas o caminho fica pronto.

⚠️ **O QUE FOI VERIFICADO E O QUE NÃO FOI.** A gramática abaixo vem do manual da
Vector versionado em `docs/referencias/` (Part 2, slides 7 a 13) e da ajuda
online do CANoe. Foi lida, não adivinhada. **Mas a saída deste emissor ainda não
foi carregada num CANoe real** — a sessão de demonstração onde a gramática foi
lida não aceita arquivos de fora. Um ponto em particular segue **não
confirmado**: o atributo de endpoint TCP. O manual só mostra `udpEndpoint`, e
`tcpEndpoint` é o nome óbvio do par, mas é inferência. Está marcado no código.

A gramática, em resumo (ver `docs/VCDL.md` para o detalhe):

    [ ...atributos globais... ] binding SOMEIP;   // SD + serialização

    namespace DataTypes {
        [fixlayout=true] struct Payload24 { array<uint8, 24> data; }
        interface Service1001 { event Payload24 Event0001; }
    }

    namespace NetworkEndPoints {
        ipstack node_10 { [mac="..."] ethernetadapter ETH { } }
        ipnep  NEP_node_10 [ethernetAdapter=node_10.ETH, address="...", prefix=24];
        ipaep  AEP_node_10 [nep=NEP_node_10, port=30501];
    }

    namespace CommunicationObjects {
        [serviceId=0x1001, version=0]
        service Service1001 : DataTypes::Service1001 {
            [eventId=0x0001] refine Event0001;
            binding SOMEIP { [groupId=0x0001] eventgroup EG1 = Event0001; }
            [instanceId=0x0001, tcpEndpoint=..., sdMulticastEndpoint=...] provider node_10;
            [clientId=0,        tcpEndpoint=..., sdMulticastEndpoint=...] consumer node_2;
        }
    }

    namespace Participants {
        participant node_10 { CommunicationObjects::Service1001.node_10 }
    }

Uso:
    python3 tools/emit_vcdl.py --matrix platform/comm_matrix_kim.yaml --out build/cfg/matriz.vCDL
"""
from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_config  # noqa: E402  (reusa endpoints_de, transporte_de e consumos_de)
import matrix_table  # noqa: E402

VERSAO_VCDL = "2.0"


def mac_de(ip: str) -> str:
    """MAC sintético derivado do endereço, estável e único por nó.

    O CANoe exige um MAC por `ipstack`. A matriz não tem MAC — a captura do
    barramento de referência tem, mas o `extract_matrix.py` não o recupera porque
    endereço de enlace não entra em nenhuma das 16 features. Derivar do IP mantém
    a correspondência um-para-um e deixa claro, olhando, de que nó se trata.
    """
    partes = [int(x) for x in ip.split(".")]
    return "02:00:%02X:%02X:%02X:%02X" % tuple(partes)


def nome_payload(tamanho: int) -> str:
    return "Payload%d" % tamanho


def bloco_global(rede: dict) -> list:
    """Atributos globais de Service Discovery e serialização.

    Os valores de SD vêm da matriz, que por sua vez os recuperou da captura. Os
    de serialização são os do exemplo da Vector; a captura não os expõe e mudá-los
    sem medir seria inventar.
    """
    sd = rede.get("service_discovery", {}) or {}
    ciclo = float(sd.get("cyclic_offer_delay", 2000)) / 1000.0
    base = float(sd.get("repetitions_base_delay", 200)) / 1000.0
    atrasoMin = float(sd.get("initial_delay_min", 10)) / 1000.0
    atrasoMax = float(sd.get("initial_delay_max", 100)) / 1000.0
    ttl = int(sd.get("ttl", 3))

    return [
        "/* Ajustes globais de SOME/IP. Os de Service Discovery vem da matriz; os de",
        " * serializacao sao os do exemplo da Vector, porque a captura nao os expoe. */",
        "[",
        "  prbslayer=\"Ethernet\"",
        ", offerTTL=%ds" % ttl,
        ", initialOfferDelayMin=%gs" % atrasoMin,
        ", initialOfferDelayMax=%gs" % atrasoMax,
        ", offerRepetitionsBaseDelay=%gs" % base,
        ", offerRepetitionsMax=%d" % int(sd.get("repetitions_max", 3)),
        ", offerCycleTime=%gs" % ciclo,
        ", findTTL=%ds" % ttl,
        ", alignment=4",
        ", serializationByteOrder=\"Motorola\"",
        ", arrayLengthFieldSize=0",
        ", structLengthFieldSize=0",
        "]",
        "binding SOMEIP;",
        "",
    ]


def bloco_tipos(matrix: dict) -> list:
    """Structs de payload e as interfaces de servico.

    Um struct por TAMANHO distinto de payload, nao por evento: o barramento de
    referencia tem onze eventos e apenas quatro tamanhos, e repetir a mesma
    definicao quatro vezes nao acrescenta nada. `fixlayout=true` e o que faz o
    struct ter layout binario fixo, sem campo de comprimento na frente -- que e o
    que o payload cru do barramento e.
    """
    tamanhos = set()
    for svc in matrix["services"].values():
        for ev in svc["events"]:
            tamanhos.add(int(ev.get("payload_bytes") or 8))

    L = ["namespace DataTypes", "{"]
    L.append("    // Um struct por tamanho de payload. fixlayout=true: layout binario fixo,")
    L.append("    // sem campo de comprimento -- e o que o payload cru do barramento e.")
    for t in sorted(tamanhos):
        L.append("    [fixlayout=true]")
        L.append("    struct %s { array<uint8, %d> data; }" % (nome_payload(t), t))
    L.append("")

    for nome, svc in matrix["services"].items():
        L.append("    interface %s" % nome)
        L.append("    {")
        for ev in svc["events"]:
            tam = int(ev.get("payload_bytes") or 8)
            L.append("        event %s %s;" % (nome_payload(tam), ev["name"]))
        L.append("    }")
        L.append("")
    L.append("}")
    L.append("")
    return L


def bloco_endpoints(matrix: dict) -> list:
    """Pilhas IP, endpoints de rede e endpoints de aplicacao, um conjunto por no."""
    rede = matrix["network"]
    sd = rede.get("service_discovery", {}) or {}
    mcast = sd.get("multicast", "224.244.224.245")
    porta_sd = int(sd.get("port", 30490))
    prefixo = int(str(rede.get("subnet", "0.0.0.0/24")).split("/")[-1])

    L = ["namespace NetworkEndPoints", "{"]
    for nome, no in matrix["nodes"].items():
        L.append("    ipstack %s" % nome)
        L.append("    {")
        L.append("        [mac=\"%s\"]" % mac_de(no["ip"]))
        L.append("        ethernetadapter ETH")
        L.append("        {")
        L.append("        }")
        L.append("    }")
    L.append("")

    for nome, no in matrix["nodes"].items():
        L.append("    ipnep NEP_%s [ethernetAdapter=%s.ETH, address=\"%s\", prefix=%d];"
                 % (nome, nome, no["ip"], prefixo))
        L.append("    ipnep NEPSD_%s [ethernetAdapter=%s.ETH, address=\"%s\"];" % (nome, nome, mcast))
    L.append("")

    for nome in matrix["nodes"]:
        L.append("    ipaep AEPSD_%s [nep=NEPSD_%s, port=%d];" % (nome, nome, porta_sd))

    # Um endpoint de aplicacao por (no, servico): a porta e a do SERVICO, porque e
    # ela que a oferta de SD anuncia e e por ela que o trafego ciclico passa.
    for nomeSvc, svc in matrix["services"].items():
        eps = gen_config.endpoints_de(svc)
        transporte = gen_config.transporte_de(svc)
        porta = eps.get("tcp" if transporte == "Reliable" else "udp") or next(iter(eps.values()))
        for nome in matrix["nodes"]:
            L.append("    ipaep AEP_%s_%s [nep=NEP_%s, port=%d];" % (nome, nomeSvc, nome, porta))
    L.append("}")
    L.append("")
    return L


def bloco_servicos(matrix: dict) -> list:
    """Binding SOME/IP: identificadores, eventgroups, provedores e consumidores."""
    L = ["namespace CommunicationObjects", "{"]

    for nomeSvc, svc in matrix["services"].items():
        transporte = gen_config.transporte_de(svc)
        # ⚠️ NAO CONFIRMADO: o manual da Vector so mostra `udpEndpoint`. `tcpEndpoint`
        # e o nome obvio do par, mas e inferencia -- verificar num CANoe real antes
        # de confiar. O resto deste arquivo foi lido do manual.
        chaveEp = "tcpEndpoint" if transporte == "Reliable" else "udpEndpoint"

        L.append("    [serviceId=0x%04X, version=0]" % svc["id"])
        L.append("    service %s : DataTypes::%s" % (nomeSvc, nomeSvc))
        L.append("    {")
        for ev in svc["events"]:
            L.append("        [eventId=0x%04X]" % ev["id"])
            L.append("        refine %s;" % ev["name"])
        L.append("")

        grupos = svc.get("eventgroups") or []
        if grupos:
            L.append("        binding SOMEIP")
            L.append("        {")
            for g in grupos:
                L.append("            [groupId=0x%04X, multicastThreshold=0]" % g["id"])
                L.append("            eventgroup EG%04X = %s;" % (g["id"], ", ".join(g["events"])))
            L.append("        }")
            L.append("")

        for nome, no in matrix["nodes"].items():
            if nomeSvc in (no.get("provides") or []):
                L.append("        [instanceId=0x%04X, %s=NetworkEndPoints::AEP_%s_%s,"
                         % (svc["instance"], chaveEp, nome, nomeSvc))
                L.append("         sdMulticastEndpoint=NetworkEndPoints::AEPSD_%s]" % nome)
                L.append("        provider %s;" % nome)

        cliente = 0
        for nome, no in matrix["nodes"].items():
            if nomeSvc in matrix_table.consumos_de(no):
                L.append("        [clientId=%d, %s=NetworkEndPoints::AEP_%s_%s,"
                         % (cliente, chaveEp, nome, nomeSvc))
                L.append("         sdMulticastEndpoint=NetworkEndPoints::AEPSD_%s]" % nome)
                L.append("        consumer %s;" % nome)
                cliente += 1

        L.append("    }")
        L.append("")
    L.append("}")
    L.append("")
    return L


def bloco_participantes(matrix: dict) -> list:
    """Um participante por no, com os endpoints de servico que ele tem."""
    L = ["namespace Participants", "{"]
    for nome, no in matrix["nodes"].items():
        pontos = []
        for s in (no.get("provides") or []):
            pontos.append("CommunicationObjects::%s.%s" % (s, nome))
        for s in matrix_table.consumos_de(no):
            pontos.append("CommunicationObjects::%s.%s" % (s, nome))
        if not pontos:
            # O monitor nem oferece nem consome: ele captura. Participante sem
            # endpoint nao tem o que declarar, e declarar vazio confundiria.
            continue
        L.append("    participant %s" % nome)
        L.append("    {")
        for p in pontos:
            L.append("        %s" % p)
        L.append("    }")
    L.append("}")
    return L


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--matrix", default="platform/comm_matrix.yaml")
    ap.add_argument("--out", default="build/cfg/matriz.vCDL")
    a = ap.parse_args()

    import yaml

    with open(a.matrix, encoding="utf-8") as fh:
        matrix = yaml.safe_load(fh)

    linhas = []
    linhas.append("/* GERADO por tools/emit_vcdl.py a partir de %s" % os.path.basename(a.matrix))
    linhas.append(" *")
    linhas.append(" * Mesma matriz que sobe os nos em vsomeip, no formato do Vector CANoe.")
    linhas.append(" * Um barramento, duas implementacoes.")
    linhas.append(" *")
    linhas.append(" * ⚠️ AINDA NAO CARREGADO NUM CANoe REAL. A gramatica foi lida do manual da")
    linhas.append(" * Vector (docs/referencias/), nao adivinhada, mas a saida nao foi validada.")
    linhas.append(" * O atributo de endpoint TCP em particular e inferencia. Ver docs/VCDL.md. */")
    linhas.append("version %s;" % VERSAO_VCDL)
    linhas.append("")
    linhas += bloco_global(matrix["network"])
    linhas += bloco_tipos(matrix)
    linhas += bloco_endpoints(matrix)
    linhas += bloco_servicos(matrix)
    linhas += bloco_participantes(matrix)

    destino = os.path.dirname(os.path.abspath(a.out))
    os.makedirs(destino, exist_ok=True)
    with open(a.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(linhas) + "\n")

    print("%d servicos, %d nos -> %s (%d linhas)"
          % (len(matrix["services"]), len(matrix["nodes"]), a.out, len(linhas)))


if __name__ == "__main__":
    main()
