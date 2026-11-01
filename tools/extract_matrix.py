#!/usr/bin/env python3
"""Recupera a matriz de comunicação de um barramento a partir de uma CAPTURA.

A captura é a especificação. Tudo que a bancada precisa para reproduzir um
barramento está no PCAP dele: quem oferece qual serviço, em que porta e
transporte, quem assina qual eventgroup, quais eventos existem, com que cadência
e com que tamanho de payload.

Este extrator lê isso e emite uma `comm_matrix.yaml` no formato que o
`tools/gen_config.py` consome. Com ele a bancada deixa de gerar "um tráfego
SOME/IP" e passa a **reproduzir um barramento de referência** -- o que dá uma
condição de controle para medir desvio de domínio variando uma coisa por vez,
em vez de mudar tudo de uma vez e não poder atribuir causa.

É a mesma lógica que validou o gerador na Fase 0: conferir contra algo
conhecidamente correto antes de inovar em cima.

⚠️ O que NÃO é recuperável, e é dito no YAML gerado:
  - os NOMES das funções (ADAS, GNSS, IMU...). O barramento carrega
    identificadores, não semântica. Os nomes saem sintéticos;
  - a lógica que produz o conteúdo do payload. Recuperam-se tamanho e
    distribuição de bytes, não a função que os gerou;
  - o jitter de escalonamento.

Uso:
    python3 tools/extract_matrix.py --pcap benign_traffic.pcap \\
            --out platform/comm_matrix_kim.yaml
"""
from __future__ import annotations

import argparse
import collections
import socket
import statistics
import struct
import sys

SD_PORT = 30490
MT_NOTIFICATION = 0x02
SD_OFFER = 0x01
SD_SUBSCRIBE = 0x06
SD_SUBSCRIBE_ACK = 0x07
OPT_IPV4_ENDPOINT = 0x04
PROTO_TCP = 6
PROTO_UDP = 17
ENTRY_BYTES = 16
SOMEIP_HEADER = 16


class Barramento:
    """Tudo que a varredura acumula sobre o barramento."""

    def __init__(self):
        self.ofertas = {}                                   # service -> dict
        self.assinaturas = collections.defaultdict(set)     # service -> {(ip, eventgroup)}
        self.eventos = collections.defaultdict(dict)        # service -> event -> dict
        self.recebe = collections.defaultdict(set)          # (service, event) -> {ip destino}
        self.ips = set()
        self.subnets = collections.Counter()
        self.sd_multicast = collections.Counter()
        self.pacotes = 0
        self.quadros = 0


def le_pcap(caminho, limite=None):
    """Itera (timestamp, bytes) de um PCAP clássico, little-endian."""
    with open(caminho, "rb") as fh:
        cab = fh.read(24)
        if len(cab) < 24:
            raise SystemExit("arquivo curto demais para ser um PCAP")
        magic = struct.unpack("<I", cab[0:4])[0]
        if magic != 0xA1B2C3D4:
            raise SystemExit("magic 0x%08X: este extrator le PCAP classico little-endian" % magic)
        rede = struct.unpack("<I", cab[20:24])[0]
        if rede != 1:
            raise SystemExit("link type %d: este extrator le Ethernet (1)" % rede)
        n = 0
        while True:
            h = fh.read(16)
            if len(h) < 16:
                return
            seg, useg, cap, _fio = struct.unpack("<IIII", h)
            raw = fh.read(cap)
            if len(raw) < cap:
                return
            n += 1
            if limite and n > limite:
                return
            yield (seg + useg * 1e-6), raw


def opcoes_sd(sd, inicio):
    """Lê a secao de opcoes do SD e devolve as opcoes de endpoint IPv4, em ordem.

    Formato, conferido contra os bytes de uma oferta real desta captura:

        0009 04 00 ac120005 00 06 7726
        |    |  |  |        |  |  +-- porta (2)
        |    |  |  |        |  +----- L4 proto (1): 06 TCP, 11 UDP
        |    |  |  |        +-------- reservado (1)
        |    |  |  +----------------- endereco IPv4 (4)
        |    |  +-------------------- reservado (1)
        |    +----------------------- tipo (1): 04 = endpoint IPv4
        +---------------------------- length (2) = 9

    O `length` NAO conta os campos de length e de tipo: ele comeca no byte
    reservado. Logo a opcao inteira ocupa 3 + length bytes, e o corpo util sao os
    `length` bytes a partir de i+3. A primeira versao disto usava `tam-1` nos dois
    lugares, o que devolvia porta zero e desalinhava a opcao seguinte.
    """
    saida = []
    if (inicio + 4) > len(sd):
        return saida
    comprimento = struct.unpack(">I", sd[inicio:inicio + 4])[0]
    i = inicio + 4
    fim = min(i + comprimento, len(sd))
    while (i + 3) <= fim:
        tam = struct.unpack(">H", sd[i:i + 2])[0]
        tipo = sd[i + 2]
        corpo = sd[i + 3:i + 3 + tam]
        if (tipo == OPT_IPV4_ENDPOINT) and (len(corpo) >= 9):
            saida.append({
                "ip": socket.inet_ntoa(corpo[1:5]),
                "proto": corpo[6],
                "port": struct.unpack(">H", corpo[7:9])[0],
            })
        if tam == 0:
            break
        i += 3 + tam
    return saida


def processa_sd(bus, src, body):
    """Extrai ofertas e assinaturas de um payload de Service Discovery."""
    if len(body) < 12:
        return
    bus.sd_multicast[src] += 0  # mantém a chave, a contagem real vem do destino
    len_entries = struct.unpack(">I", body[4:8])[0]
    i = 8
    fim = min(8 + len_entries, len(body))
    opcoes = opcoes_sd(body, fim)
    while (i + ENTRY_BYTES) <= fim:
        tipo = body[i]
        n_opt1 = body[i + 3] >> 4
        idx1 = body[i + 1]
        servico, instancia = struct.unpack(">HH", body[i + 4:i + 8])
        maior = body[i + 8]
        ttl = int.from_bytes(body[i + 9:i + 12], "big")

        if (tipo == SD_OFFER) and (ttl > 0):
            registro = bus.ofertas.setdefault(servico, {
                "instance": instancia, "major": maior, "ttl": ttl,
                "provider": src, "endpoints": {},
            })
            # As portas vêm das OPÇÕES DE ENDPOINT da própria oferta, não de
            # adivinhar pelo tráfego observado. E são VÁRIAS: nesta captura cada
            # serviço anuncia dois endpoints, um TCP e um UDP. Guardar só o
            # último sobrescreveria um pelo outro.
            for k in range(n_opt1):
                if (idx1 + k) < len(opcoes):
                    op = opcoes[idx1 + k]
                    registro["endpoints"][op["proto"]] = op["port"]
        elif tipo in (SD_SUBSCRIBE, SD_SUBSCRIBE_ACK):
            grupo = struct.unpack(">H", body[i + 14:i + 16])[0]
            if tipo == SD_SUBSCRIBE:
                # Só o Subscribe conta como assinante. O Ack vem do PROVEDOR, e
                # confundir os dois faz o provedor aparecer assinando o próprio
                # serviço -- erro que a primeira leitura desta captura cometeu.
                bus.assinaturas[servico].add((src, grupo))
        i += ENTRY_BYTES


def varre(caminho, limite=None):
    """Passa pela captura e acumula tudo."""
    bus = Barramento()
    ultimo = {}
    intervalos = collections.defaultdict(list)
    tamanhos = collections.defaultdict(collections.Counter)

    for ts, raw in le_pcap(caminho, limite):
        bus.quadros += 1
        if (len(raw) < 34) or (raw[12:14] != b"\x08\x00"):
            continue
        ihl = (raw[14] & 0x0F) * 4
        proto = raw[23]
        l4 = 14 + ihl
        src = socket.inet_ntoa(raw[26:30])
        dst = socket.inet_ntoa(raw[30:34])

        if proto == PROTO_UDP:
            if len(raw) < l4 + 8:
                continue
            sport, dport = struct.unpack(">HH", raw[l4:l4 + 4])
            off = l4 + 8
        elif proto == PROTO_TCP:
            if len(raw) < l4 + 13:
                continue
            sport, dport = struct.unpack(">HH", raw[l4:l4 + 4])
            off = l4 + ((raw[l4 + 12] >> 4) * 4)
        else:
            continue

        bus.ips.add(src)
        bus.subnets[".".join(src.split(".")[:3])] += 1
        corpo = raw[off:]
        if len(corpo) < SOMEIP_HEADER:
            continue
        bus.pacotes += 1
        servico, metodo = struct.unpack(">HH", corpo[0:4])
        tipo_msg = corpo[14]
        carga = corpo[SOMEIP_HEADER:]

        if (servico == 0xFFFF) or (dport == SD_PORT) or (sport == SD_PORT):
            if dst.startswith("224."):
                bus.sd_multicast[dst] += 1
            processa_sd(bus, src, carga)
            continue

        if tipo_msg == MT_NOTIFICATION:
            chave = (servico, metodo)
            bus.recebe[chave].add(dst)
            tamanhos[chave][len(carga)] += 1
            if chave in ultimo:
                intervalos[chave].append(ts - ultimo[chave])
            ultimo[chave] = ts
            bus.eventos[servico].setdefault(metodo, {})

    for (servico, evento), amostras in intervalos.items():
        if servico in bus.eventos and evento in bus.eventos[servico]:
            bus.eventos[servico][evento]["cycle_ms"] = round(statistics.median(amostras) * 1000.0, 1)
            bus.eventos[servico][evento]["n"] = len(amostras) + 1
    for (servico, evento), contagem in tamanhos.items():
        if servico in bus.eventos and evento in bus.eventos[servico]:
            comum, quantos = contagem.most_common(1)[0]
            bus.eventos[servico][evento]["payload_bytes"] = comum
            bus.eventos[servico][evento]["payload_fixo"] = (quantos == sum(contagem.values()))
    return bus


def grupos_por_evento(bus):
    """Associa cada eventgroup aos eventos que ele carrega.

    O SD diz quem assina qual eventgroup, mas não quais eventos estão nele. A
    associação sai por CORRELAÇÃO: o conjunto de destinos que recebem um evento
    tem de ser IGUAL ao conjunto de assinantes de um eventgroup.

    Igualdade, e não "contido em": com "contido" um evento entregue a um único
    destino casa com todo grupo que aquele destino assinou, e a primeira versão
    disto reportou quatro eventgroups para um serviço de quatro eventos, um por
    evento, que é o sintoma de uma regra que sempre casa.

    Um evento que não casa com grupo nenhum é reportado à parte, em vez de ser
    atribuído a um palpite.
    """
    saida = collections.defaultdict(lambda: collections.defaultdict(set))
    orfaos = collections.defaultdict(set)
    for servico, pares in bus.assinaturas.items():
        assinantes_de = collections.defaultdict(set)
        for ip, grupo in pares:
            assinantes_de[grupo].add(ip)
        for evento in bus.eventos.get(servico, {}):
            destinos = {d for d in bus.recebe[(servico, evento)] if not d.startswith("224.")}
            if not destinos:
                continue
            casou = False
            for grupo, assinantes in assinantes_de.items():
                if destinos == assinantes:
                    saida[servico][grupo].add(evento)
                    casou = True
            if not casou:
                orfaos[servico].add(evento)
    return saida, orfaos


def emite(bus, grupos, orfaos, caminho, origem):
    """Escreve o YAML no formato que o tools/gen_config.py consome."""
    sub = bus.subnets.most_common(1)[0][0] if bus.subnets else "172.18.0"
    mc = bus.sd_multicast.most_common(1)[0][0] if bus.sd_multicast else "224.244.224.245"
    provedores = {d["provider"] for d in bus.ofertas.values()}

    L = []
    L.append("# Matriz de comunicação RECUPERADA de uma captura, por tools/extract_matrix.py.")
    L.append("#")
    L.append("# Fonte: %s" % origem)
    L.append("# %d quadros, %d mensagens SOME/IP, %d nós." % (bus.quadros, bus.pacotes, len(bus.ips)))
    L.append("#")
    L.append("# ⚠️ NÃO EDITE à mão esperando que o barramento mude: este arquivo DESCREVE um")
    L.append("# barramento que já existe. Editar aqui faz a bancada gerar outra coisa, que é")
    L.append("# legítimo, mas aí deixa de ser reprodução e passa a ser variação -- e a variação")
    L.append("# precisa ser deliberada e anotada, senão o estudo de desvio de domínio perde o")
    L.append("# controle.")
    L.append("#")
    L.append("# ⚠️ O que a captura NÃO carrega, e portanto não está aqui:")
    L.append("#   - os nomes das funções (ADAS, GNSS, IMU...): o barramento tem identificador,")
    L.append("#     não semântica. Os nomes abaixo são sintéticos, derivados dos IDs;")
    L.append("#   - a lógica que gera o conteúdo do payload: recuperam-se tamanho e cadência,")
    L.append("#     não a função que os produziu.")
    L.append("")
    L.append("network:")
    L.append("  subnet: %s.0/24" % sub)
    L.append("  multicast_route: 224.0.0.0/4")
    L.append("  service_discovery:")
    L.append("    multicast: %s" % mc)
    L.append("    port: %d" % SD_PORT)
    L.append("    protocol: udp")
    L.append("    initial_delay_min: 10")
    L.append("    initial_delay_max: 100")
    L.append("    repetitions_base_delay: 200")
    L.append("    repetitions_max: 3")
    L.append("    ttl: 3")
    L.append("    cyclic_offer_delay: 2000")
    L.append("")
    L.append("services:")
    for servico in sorted(bus.ofertas):
        d = bus.ofertas[servico]
        eps = d["endpoints"]
        porta_tcp = eps.get(PROTO_TCP)
        porta_udp = eps.get(PROTO_UDP)
        L.append("  Service%04X:" % servico)
        L.append("    id: 0x%04X" % servico)
        L.append("    instance: 0x%04X" % d["instance"])
        # O serviço anuncia DOIS endpoints. O `port`/`protocol` ficam no confiável
        # porque é por ele que os dados andam nesta captura, e porque é o único par
        # que o tools/gen_config.py consome hoje. O outro fica registrado ao lado,
        # para a reprodução não perder informação que a captura carrega.
        if porta_tcp:
            L.append("    port: %d" % porta_tcp)
            L.append("    protocol: tcp")
        elif porta_udp:
            L.append("    port: %d" % porta_udp)
            L.append("    protocol: udp")
        L.append("    endpoints:            # anunciados na oferta de SD")
        if porta_tcp:
            L.append("      - { protocol: tcp, port: %d }" % porta_tcp)
        if porta_udp:
            L.append("      - { protocol: udp, port: %d }" % porta_udp)
        L.append("    events:")
        for evento in sorted(bus.eventos.get(servico, {})):
            e = bus.eventos[servico][evento]
            fixo = "" if e.get("payload_fixo") else "  # tamanho VARIA; este é o mais comum"
            L.append("      - { name: Event%04X, id: 0x%04X, is_field: true, cycle_ms: %s, "
                     "payload_bytes: %s }%s"
                     % (evento, evento, e.get("cycle_ms", 0), e.get("payload_bytes", 0), fixo))
        gr = grupos.get(servico, {})
        if gr:
            L.append("    eventgroups:")
            for grupo in sorted(gr):
                membros = ", ".join("Event%04X" % ev for ev in sorted(gr[grupo]))
                L.append("      - { id: 0x%04X, events: [%s] }" % (grupo, membros))
        else:
            L.append("    # nenhum eventgroup pôde ser associado por correlação")
        if orfaos.get(servico):
            soltos = ", ".join("Event%04X" % e for e in sorted(orfaos[servico]))
            L.append("    # eventos sem eventgroup por correlação: %s" % soltos)
            L.append("    # (os destinos deles nao coincidem com nenhum conjunto de assinantes;")
            L.append("    #  pode ser notificacao multicast ou assinatura fora da janela lida)")
        L.append("")

    L.append("nodes:")
    for ip in sorted(bus.ips, key=lambda s: [int(x) for x in s.split(".")]):
        ultimo_octeto = int(ip.split(".")[-1])
        oferece = [s for s, d in bus.ofertas.items() if d["provider"] == ip]
        consome = sorted({s for s, pares in bus.assinaturas.items() if any(p[0] == ip for p in pares)})
        papel = "sensor" if oferece else "consumidor"
        L.append("  node_%d:" % ultimo_octeto)
        L.append("    ip: %s" % ip)
        L.append("    app_id: 0x%04X" % (0x1000 + ultimo_octeto))
        L.append("    app_name: node-%d" % ultimo_octeto)
        L.append("    role: %s" % papel)
        if oferece:
            L.append("    provides: [%s]" % ", ".join("Service%04X" % s for s in sorted(oferece)))
        if consome:
            L.append("    consumes: [%s]" % ", ".join("Service%04X" % s for s in consome))
        L.append("")

    with open(caminho, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(L))
    return len(provedores)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pcap", required=True)
    ap.add_argument("--out", default="platform/comm_matrix_recuperada.yaml")
    ap.add_argument("--max-packets", type=int, default=None)
    a = ap.parse_args()

    bus = varre(a.pcap, a.max_packets)
    grupos, orfaos = grupos_por_evento(bus)
    n_prov = emite(bus, grupos, orfaos, a.out, a.pcap)

    print("quadros lidos        : %d" % bus.quadros)
    print("mensagens SOME/IP    : %d" % bus.pacotes)
    print("nós                  : %d" % len(bus.ips))
    print("serviços ofertados   : %d (de %d provedores)" % (len(bus.ofertas), n_prov))
    for s in sorted(bus.ofertas):
        d = bus.ofertas[s]
        eps = "  ".join("%s:%d" % ("tcp" if k == PROTO_TCP else "udp", v)
                        for k, v in sorted(d["endpoints"].items()))
        evs = bus.eventos.get(s, {})
        gs = grupos.get(s, {})
        print("   0x%04X inst 0x%04X  %s  %d eventos, %d eventgroups  provedor %s"
              % (s, d["instance"], eps, len(evs), len(gs), d["provider"]))
        for g in sorted(gs):
            print("      eventgroup 0x%04X: %s" % (g, ", ".join("0x%04X" % e for e in sorted(gs[g]))))
        if orfaos.get(s):
            print("      sem eventgroup: %s" % ", ".join("0x%04X" % e for e in sorted(orfaos[s])))
        for ev in sorted(evs):
            e = evs[ev]
            print("      evento 0x%04X  cadência %s ms  payload %s B%s  (%d notificações)"
                  % (ev, e.get("cycle_ms", "?"), e.get("payload_bytes", "?"),
                     "" if e.get("payload_fixo") else " [varia]", e.get("n", 0)))
    print()
    print("-> %s" % a.out)
    sys.exit(0)


if __name__ == "__main__":
    main()
