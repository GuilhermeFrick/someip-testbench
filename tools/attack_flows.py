#!/usr/bin/env python3
"""Mostra o que o atacante do dataset faz NO FIO, para o injetor vivo não inventar nada.

Para cada PCAP de ataque, separa os quadros pela regra do dataset (origem fora do
conjunto benigno = atacante) e responde, com contagem:

  * quem é o atacante (IPs) e por qual transporte ele fala;
  * para ONDE ele manda: IP e porta de destino, serviço, tipo de mensagem;
  * quem abre cada conexão TCP (o SYN sai de quem?);
  * o que ele diz no Service Discovery (oferta, assinatura, com qual endpoint);
  * o que os nós benignos mandam DE VOLTA para ele (se recebe notificação, assinou).

Sem dependência externa: lê o PCAP clássico direto.

Uso:
    python3 tools/attack_flows.py <pcap> [--max-frames N] [--prefix 172.28.0.]
"""
import argparse
import struct
from collections import Counter

# Último octeto de cada ECU do barramento de referência. A sub-rede vem de --prefix,
# porque a bancada pode rodar remapeada (platform/bus-kim, SUBNET=), e o remapeamento
# preserva o último octeto.
ECUS = {2: "ADAS", 3: "CLU", 4: "GPS", 5: "IMU", 6: "NAV", 7: "PT", 8: "STE", 9: "TEL", 10: "VDE"}
BENIGNOS = set()
NOMES = {}


def define_subrede(prefixo):
    BENIGNOS.clear()
    NOMES.clear()
    for octeto, ecu in ECUS.items():
        BENIGNOS.add(prefixo + str(octeto))
        NOMES[prefixo + str(octeto)] = ecu
TIPOS = {0x00: "REQUEST", 0x01: "REQUEST_NO_RETURN", 0x02: "NOTIFICATION", 0x80: "RESPONSE", 0x81: "ERROR"}
SD_ENTRADAS = {0x00: "FindService", 0x01: "OfferService", 0x06: "SubscribeEventgroup",
               0x07: "SubscribeEventgroupAck"}


def nome(ip):
    return "%s(%s)" % (ip, NOMES[ip]) if ip in NOMES else ip


def le_pcap(caminho):
    with open(caminho, "rb") as fh:
        cab = fh.read(24)
        end = "<" if struct.unpack("<I", cab[:4])[0] in (0xA1B2C3D4, 0xA1B23C4D) else ">"
        while True:
            reg = fh.read(16)
            if len(reg) < 16:
                return
            _s, _u, incl, _o = struct.unpack(end + "IIII", reg)
            yield fh.read(incl)


def quadro(raw):
    """(src, dst, proto, sport, dport, flags_tcp, corpo) de um quadro IPv4, ou None."""
    if len(raw) < 34 or raw[12:14] != b"\x08\x00":
        return None
    ihl = (raw[14] & 0x0F) * 4
    proto = raw[23]
    src = ".".join(str(b) for b in raw[26:30])
    dst = ".".join(str(b) for b in raw[30:34])
    l4 = 14 + ihl
    if proto == 17:
        sport, dport = struct.unpack(">HH", raw[l4:l4 + 4])
        return src, dst, "udp", sport, dport, "", raw[l4 + 8:]
    if proto == 6:
        sport, dport = struct.unpack(">HH", raw[l4:l4 + 4])
        fl = raw[l4 + 13]
        flags = "".join(c for c, b in (("S", 2), ("A", 16), ("F", 1), ("R", 4), ("P", 8)) if fl & b)
        return src, dst, "tcp", sport, dport, flags, raw[l4 + ((raw[l4 + 12] >> 4) * 4):]
    return None


def someip(corpo):
    i = 0
    while i + 16 <= len(corpo):
        svc, mth, ln = struct.unpack(">HHI", corpo[i:i + 8])
        fim = i + 8 + ln
        if ln < 8 or fim > len(corpo):
            return
        yield svc, mth, corpo[i + 14], corpo[i + 16:fim]
        i = fim


def sd_entradas(pl):
    """Entradas do SD com o endpoint IPv4 da primeira opção que a entrada referencia."""
    if len(pl) < 12:
        return
    n_ent = struct.unpack(">I", pl[4:8])[0]
    ent = pl[8:8 + n_ent]
    base_op = 8 + n_ent
    opcoes = []
    if base_op + 4 <= len(pl):
        n_op = struct.unpack(">I", pl[base_op:base_op + 4])[0]
        j = base_op + 4
        while j + 3 <= base_op + 4 + n_op and j + 3 <= len(pl):
            olen, otyp = struct.unpack(">HB", pl[j:j + 3])
            if otyp == 0x04 and j + 12 <= len(pl):
                ip = ".".join(str(b) for b in pl[j + 4:j + 8])
                prt = {6: "tcp", 17: "udp"}.get(pl[j + 9], "?")
                port = struct.unpack(">H", pl[j + 10:j + 12])[0]
                opcoes.append("%s %s:%d" % (prt, ip, port))
            else:
                opcoes.append("opt0x%02x" % otyp)
            j += 3 + olen
    for k in range(0, len(ent) - 15, 16):
        typ, i1 = ent[k], ent[k + 1]
        nops = ent[k + 3] >> 4
        svc = struct.unpack(">H", ent[k + 4:k + 6])[0]
        ttl = int.from_bytes(ent[k + 9:k + 12], "big")
        eps = ", ".join(opcoes[i1:i1 + nops]) if nops else "-"
        nome = SD_ENTRADAS.get(typ, "tipo0x%02x" % typ)
        # TTL zero inverte o sentido da entrada: Offer vira StopOffer, Subscribe vira
        # StopSubscribe e Ack vira Nack (recusa). Sem separar, uma rodada cheia de
        # assinaturas RECUSADAS parece cheia de assinaturas aceitas.
        if ttl == 0:
            nome = {"OfferService": "StopOfferService", "SubscribeEventgroup": "StopSubscribeEventgroup",
                    "SubscribeEventgroupAck": "SubscribeEventgroupNack"}.get(nome, nome)
        yield nome, svc, eps


def imprime(titulo, cont, n=15):
    print("  " + titulo)
    total = sum(cont.values())
    for k, v in cont.most_common(n):
        print("    %7d  %5.1f%%  %s" % (v, 100.0 * v / max(total, 1), k))
    if len(cont) > n:
        print("    ... %d outras linhas" % (len(cont) - n))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--max-frames", type=int, default=2000000)
    ap.add_argument("--prefix", default="172.18.0.", help="sub-rede do barramento, até o último ponto")
    a = ap.parse_args()
    define_subrede(a.prefix)

    atk_ips, transporte, destino, syn, sd, volta, rst = (Counter() for _ in range(7))
    n = 0
    for idx, raw in enumerate(le_pcap(a.pcap)):
        if idx >= a.max_frames:
            break
        q = quadro(raw)
        if q is None:
            continue
        src, dst, proto, sport, dport, flags, corpo = q
        de_atk, para_atk = src not in BENIGNOS, dst not in BENIGNOS and not dst.startswith("224.")
        if proto == "tcp" and flags == "S":
            syn["%s -> %s:%d" % (nome(src), nome(dst), dport)] += 1
        if de_atk:
            n += 1
            atk_ips[src] += 1
            transporte[proto] += 1
            msgs = list(someip(corpo))
            if not msgs:
                destino["%s %s:%d  [%s, sem SOME/IP]" % (proto, nome(dst), dport, flags or "-")] += 1
            for svc, mth, mt, pl in msgs:
                if svc == 0xFFFF:
                    for tipo, s2, eps in sd_entradas(pl):
                        sd["%s serviço 0x%04x  endpoint %s  -> %s" % (tipo, s2, eps, nome(dst))] += 1
                else:
                    destino["%s %s:%d  serviço 0x%04x %s" % (proto, nome(dst), dport, svc,
                                                            TIPOS.get(mt, "0x%02x" % mt))] += 1
            if proto == "tcp" and "R" in flags:
                rst["RST %s -> %s:%d" % (nome(src), nome(dst), dport)] += 1
        elif para_atk:
            msgs = list(someip(corpo))
            rot = ", ".join(sorted({"0x%04x %s" % (s, TIPOS.get(t, "SD" if s == 0xFFFF else "0x%02x" % t))
                                    for s, _m, t, _p in msgs})) or ("[%s, sem SOME/IP]" % (flags or "-"))
            volta["%s %s -> %s:%d  %s" % (proto, nome(src), dst, dport, rot)] += 1

    print("=" * 78)
    print(a.pcap, "| quadros do atacante:", n)
    imprime("IPs do atacante", atk_ips)
    imprime("transporte", transporte)
    imprime("destinos do atacante (transporte, IP:porta, serviço, tipo)", destino, 25)
    imprime("Service Discovery enviado pelo atacante", sd, 20)
    imprime("quem abre conexão TCP (SYN), todos os quadros", syn, 25)
    imprime("o que os nós benignos mandam PARA o atacante", volta, 20)
    if rst:
        imprime("RST do atacante", rst, 10)


if __name__ == "__main__":
    main()
