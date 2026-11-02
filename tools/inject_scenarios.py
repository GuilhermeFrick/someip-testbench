#!/usr/bin/env python3
"""Injeta os quatro padrões de tráfego adversarial do baseline sobre uma captura.

Por que pós-captura, e não um nó vivo.

A bancada mede hoje só FALSO POSITIVO — tráfego benigno, nenhum padrão adversarial
no barramento. Para medir DETECÇÃO é preciso tráfego rotulado das quatro famílias
do dataset. Este tool produz esse tráfego injetando os padrões sobre a captura
benigna que a bancada já gera.

Injetar pós-captura é o MESMO MÉTODO dos dois baselines, não um atalho: o Luo
injeta por script CAPL embutido no nó de emulação e admite "this is not an actual
attack scenario, but the same attack effect can be obtained"; o dataset do Kim é um
conjunto de PCAPs rotulados. A diferença da bancada é o SUBSTRATO — aqui o tráfego
benigno é a reprodução, em vsomeip real, de um barramento validado pela academia.

⚠️ AS ASSINATURAS VÊM DO DATASET REAL, NÃO DE UM GERADOR. O detector da C1 treina
direto nos PCAPs do Kim (`src/extract.py`: ATTACK_FILES), rotulados por IP de
origem. Logo o que importa reproduzir é o que o dataset CONTÉM, medido isolando as
frames de origem ∉ benigna em cada arquivo:

    familia       transporte       serviço observado        o que distingue
    DoS           ~60% UDP/40% TCP  0x1001 (legítimo)        rajada de alta taxa
    Fuzzy         UDP              aleatório, fora da faixa  esparso, payload rand.
    MITM single   UDP-pesado       0x1001 (replay)          1 cópia por evento
    MITM multi    TCP-pesado       0x100B (relay)            várias origens

Isto corrige o nosso próprio gerador de apoio (`someip-traffic-simulator`), que
usava relay 0x100B para todo MITM: no dataset só o MULTI usa 0x100B; o SINGLE
impersona o serviço legítimo. A diferença é observável nas features de conteúdo, e
por isso importa para a detecção.

⚠️ O ground truth é por ORIGEM: o atacante usa um endereço FORA do conjunto
benigno, e toda frame dele leva a classe do cenário. É a regra do `extract.py` de
referência (`src ∉ benign_ips`), a mesma que rotula o dataset. Nenhuma heurística
sobre o PCAP.

Esta ferramenta reproduz a ASSINATURA DE FRAME de cada família (serviço,
transporte, tipo, cadência), não um modelo do mecanismo interno do ataque — é o que
a medição de detecção precisa.

Saída:
    <saida>.pcap     benigno + injetado, ordenado por timestamp
    <saida>.labels   uma linha por frame: "<indice> <classe>"
                     (0 normal, 1 dos, 2 fuzzy, 3 mitm_single, 4 mitm_multi)

Uso:
    python3 tools/inject_scenarios.py --repro <repo> --pcap benigno.pcap \\
            --scenario dos --out build/scn/dos
"""
from __future__ import annotations

import argparse
import os
import random
import struct
import sys

# As classes, na ORDEM do detector (ids_sensor/Types.hpp): a rotulagem indexa esta
# lista direto, então reordenar remapearia silenciosamente toda a verdade.
CLASSES = {"normal": 0, "dos": 1, "fuzzy": 2, "mitm_single": 3, "mitm_multi": 4}

MSG_NOTIFICATION = 0x02
FUZZY_SVC_LO, FUZZY_SVC_HI = 0x2000, 0x9000
FUZZY_PLEN_MAX = 40
RELAY_SERVICE = 0x100B

# Perfil por cenário, cada campo medido das frames de origem atacante no dataset.
# udp_p: fração de frames sobre UDP (o resto TCP). source_count: origens distintas.
# burst/iat: rajada por gatilho (DoS) ou 1 cópia por gatilho (MITM). target diz de
# onde sai o cabeçalho: "copy" copia o do evento legítimo; "relay" troca o serviço
# por 0x100B mantendo o payload; "fuzzy" sorteia serviço, método e payload.
# `every` calibrado para a FRAÇÃO de ataque medida no dataset real (origem ∉ benigna):
# DoS 15,3%, Fuzzy 0,2%, MITM single 19,5%, MITM multi 17,2%. Isso é reproduzir a
# densidade do dataset, NÃO ajustar para a detecção parecer boa -- a fração é uma
# propriedade medida do baseline, como o são o serviço e o transporte.
PERFIS = {
    "dos":         {"udp_p": 0.60, "sources": 1, "burst": 40, "iat": 0.0002, "target": "copy",  "every": 85},
    "fuzzy":       {"udp_p": 1.00, "sources": 1, "burst": 12, "iat": 0.0010, "target": "fuzzy", "every": 2000},
    "mitm_single": {"udp_p": 0.70, "sources": 1, "burst": 1,  "iat": 0.0002, "target": "copy",  "every": 2},
    "mitm_multi":  {"udp_p": 0.02, "sources": 3, "burst": 1,  "iat": 0.0002, "target": "relay", "every": 2},
}

# Endereços do atacante: FORA do conjunto benigno da bancada (172.28.0.2-.10, mais
# .1 do gateway), para a rotulagem por origem funcionar.
ATTACKERS = ["172.28.0.200", "172.28.0.201", "172.28.0.202"]


def carrega_referencia(repro: str):
    """Importa o parser de referência: a definição do que é uma frame SOME/IP."""
    src = os.path.join(os.path.expanduser(repro), "src")
    if not os.path.isdir(src):
        raise SystemExit(f"--repro nao tem src/: {src}")
    sys.path.insert(0, src)
    import someip as someip_mod  # noqa: E402
    return someip_mod


def someip_bytes(service: int, method: int, payload: bytes, session: int) -> bytes:
    """Cabeçalho SOME/IP de 16 bytes mais payload, NOTIFICATION.

    Layout exato que o parser de referência lê: service(2) method(2) length(4)
    client(2) session(2) protover(1) ifver(1) msgtype(1) retcode(1). O length cobre
    de client em diante: 8 + len(payload).
    """
    length = 8 + len(payload)
    return (
        struct.pack(">HH", service, method)
        + struct.pack(">I", length)
        + struct.pack(">HH", 0, session)
        + bytes([0x01, 0x01, MSG_NOTIFICATION, 0x00])
        + payload
    )


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repro", required=True, help="repo de reproducao (parser someip.py)")
    ap.add_argument("--pcap", required=True, help="captura BENIGNA da bancada")
    ap.add_argument("--scenario", required=True, choices=list(PERFIS))
    ap.add_argument("--out", required=True, help="prefixo de saida (.pcap e .labels)")
    ap.add_argument("--seed", type=int, default=20260101, help="semente, para a rodada ser repetivel")
    a = ap.parse_args()

    someip_mod = carrega_referencia(a.repro)
    pcap = os.path.expanduser(a.pcap)
    if not os.path.exists(pcap):
        raise SystemExit(f"PCAP ausente: {pcap}")

    random.seed(a.seed)
    perfil = PERFIS[a.scenario]
    classe = CLASSES[a.scenario]
    atacantes = ATTACKERS[: perfil["sources"]]

    from scapy.all import Ether, IP, UDP, TCP, Raw, PcapWriter
    from scapy.utils import RawPcapReader

    def monta_frame(src_ip: str, dst_ip: str, dport: int, corpo: bytes) -> bytes:
        l4 = UDP(sport=random.randint(1024, 65535), dport=dport) if random.random() < perfil["udp_p"] \
            else TCP(sport=random.randint(1024, 65535), dport=dport, flags="PA")
        return bytes(Ether() / IP(src=src_ip, dst=dst_ip) / l4 / Raw(corpo))

    # 1) lê o benigno; guarda (ts, bytes, classe=normal) e as NOTIFICATION legitimas.
    frames = []
    gatilhos = []
    print(f"lendo {os.path.basename(pcap)}...")
    for raw, meta in RawPcapReader(pcap):
        ts = meta.sec + meta.usec * 1e-6
        p = someip_mod.parse(raw)
        idx = len(frames)
        frames.append([ts, bytes(raw), CLASSES["normal"]])
        if (p is not None) and p.is_someip and (not p.is_sd) and (p.msg_type == MSG_NOTIFICATION):
            gatilhos.append(idx)
    print(f"  {len(frames)} frames benignas, {len(gatilhos)} NOTIFICATION de gatilho")

    # 2) a cada `every` gatilhos, reproduz a assinatura do cenario.
    injetadas = []
    disparo = 0
    for g in range(0, len(gatilhos), perfil["every"]):
        p0 = someip_mod.parse(frames[gatilhos[g]][1])
        ts0 = frames[gatilhos[g]][0]
        atk = atacantes[disparo % len(atacantes)]
        disparo += 1
        for i in range(perfil["burst"]):
            if perfil["target"] == "copy":
                corpo = someip_bytes(p0.service_id, p0.method_id, bytes(p0.pl_someip), i & 0xFFFF)
            elif perfil["target"] == "relay":
                corpo = someip_bytes(RELAY_SERVICE, p0.method_id, bytes(p0.pl_someip), i & 0xFFFF)
            else:  # fuzzy
                plen = random.randint(0, FUZZY_PLEN_MAX)
                corpo = someip_bytes(random.randint(FUZZY_SVC_LO, FUZZY_SVC_HI),
                                     random.randint(0, 0xFFFF),
                                     bytes(random.randint(0, 255) for _ in range(plen)),
                                     i & 0xFFFF)
            fr = monta_frame(atk, p0.dst, p0.dport, corpo)
            injetadas.append([ts0 + i * perfil["iat"], fr, classe])
    print(f"  {len(injetadas)} frames injetadas ({a.scenario}, {len(atacantes)} origem(ns))")

    # 3) mescla por timestamp, estavel, e escreve PCAP + labels.
    todas = frames + injetadas
    todas.sort(key=lambda r: r[0])

    os.makedirs(os.path.dirname(os.path.abspath(a.out)) or ".", exist_ok=True)
    pcap_out, labels_out = a.out + ".pcap", a.out + ".labels"
    from scapy.all import Ether as _E
    w = PcapWriter(pcap_out, linktype=1, sync=True)  # DLT_EN10MB
    with open(labels_out, "w", encoding="utf-8", newline="\n") as lf:
        for i, (ts, raw, cls) in enumerate(todas):
            pkt = _E(raw)
            pkt.time = ts
            w.write(pkt)
            lf.write("%d %d\n" % (i, cls))
    w.close()

    n_atk = sum(1 for _, _, c in todas if c != 0)
    print(f"\n{len(todas)} frames -> {pcap_out}")
    print(f"  {n_atk} rotuladas {a.scenario} ({100.0 * n_atk / len(todas):.2f}%)")
    print(f"  labels -> {labels_out}")


if __name__ == "__main__":
    main()
