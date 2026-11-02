#!/usr/bin/env python3
"""Configuração vsomeip do injetor de cenários adversariais.

O injetor é uma ECU como as outras, e por isso precisa de uma configuração
vsomeip. Ela não sai da matriz de comunicação, porque a matriz descreve o
barramento LEGÍTIMO: o injetor é justamente o que não está nela. Cada cenário
reproduz o que o PCAP do dataset do Kim mostra para aquele atacante
(tools/attack_flows.py, tools/offer_cadence.py, platform/dup-offer):

  dos    a configuração da ECU personificada (a VDE, por padrão), com OUTRO
         unicast e a oferta cíclica a cada 20 ms, que é o ritmo medido do .11.
         Mesmo serviço, mesma instância, mesmas portas, mesmos eventos.

  fuzzy  200 serviços de 0x2001 a 0x20C8, instância igual ao número do serviço,
         portas TCP 30500+n e UDP 31096+n (n = serviço - 0x2000), oferta no ritmo
         normal do SD. É o que o .17 anuncia; nenhum deles publica eventos.

A seção service-discovery vem da configuração gerada para a ECU personificada,
para o injetor falar o mesmo SD que o barramento (multicast, porta, TTL).

Uso:
    python3 tools/gen_injector_config.py --cfg build/run/cfg --scenario dos \\
            --ip 172.28.0.11 --out build/run/cfg/vsomeip-injector.json
"""
import argparse
import copy
import json
import os
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cfg", required=True, help="diretório gerado por tools/gen_config.py")
    ap.add_argument("--scenario", required=True, choices=["dos", "mitm_single", "fuzzy"])
    ap.add_argument("--ip", required=True, help="unicast do injetor, fora das 9 ECUs")
    ap.add_argument("--out", required=True)
    ap.add_argument("--impersonate", default="node_10", help="ECU cuja configuração o dos copia")
    ap.add_argument("--cycle-ms", type=int, default=None,
                    help="oferta cíclica; padrão 20 no dos (medido) e o do barramento no fuzzy")
    ap.add_argument("--fuzz-first", type=lambda x: int(x, 0), default=0x2001)
    ap.add_argument("--fuzz-count", type=int, default=200)
    ap.add_argument("--app", default="injector")
    ap.add_argument("--app-id", type=lambda x: int(x, 0), default=0x1EED)
    a = ap.parse_args()

    base_path = os.path.join(a.cfg, "vsomeip-%s.json" % a.impersonate)
    if not os.path.exists(base_path):
        sys.exit("ERRO: %s não existe; rode tools/gen_config.py antes" % base_path)
    with open(base_path, encoding="utf-8") as fh:
        base = json.load(fh)

    cfg = {
        "unicast": a.ip,
        "logging": copy.deepcopy(base.get("logging", {})),
        "applications": [{"name": a.app, "id": "0x%04x" % a.app_id}],
        "routing": a.app,
        "service-discovery": copy.deepcopy(base["service-discovery"]),
    }

    if a.scenario in ("dos", "mitm_single"):
        if not base.get("services"):
            sys.exit("ERRO: %s não oferece serviço; nada a personificar" % a.impersonate)
        cfg["services"] = copy.deepcopy(base["services"])
        # Ritmos medidos no dataset (tools/offer_cadence.py): .11 a cada 20 ms, .13 a cada 10 ms.
        padrao = 20 if a.scenario == "dos" else 10
        cfg["service-discovery"]["cyclic_offer_delay"] = str(a.cycle_ms if a.cycle_ms is not None else padrao)
    else:
        servicos = []
        for svc in range(a.fuzz_first, a.fuzz_first + a.fuzz_count):
            n = svc - 0x2000
            servicos.append({
                "service": "0x%04x" % svc,
                "instance": "0x%04x" % svc,
                "unreliable": str(31096 + n),
                "reliable": {"port": str(30500 + n), "enable-magic-cookies": "false"},
            })
        cfg["services"] = servicos
        if a.cycle_ms is not None:
            cfg["service-discovery"]["cyclic_offer_delay"] = str(a.cycle_ms)

    with open(a.out, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, indent=2)
    print("[injector-cfg] %s: %d serviço(s), oferta a cada %s ms, unicast %s -> %s" % (
        a.scenario, len(cfg["services"]), cfg["service-discovery"]["cyclic_offer_delay"], a.ip, a.out))


if __name__ == "__main__":
    main()
