#!/usr/bin/env bash
# Degrau 0b: Service Discovery real entre containers, com os exemplos do vsomeip.
#
# PONTO CRITICO (descoberto na depuracao, ver README): cada no precisa de uma
# ROTA MULTICAST. O Docker nao cria rota para 224.0.0.0/4, e o vsomeip so liga o
# Service Discovery depois de ver uma. Sem ela a oferta fica em laco silencioso
# de `pending_sd_offers`, sem erro nem aviso.
set -u

NET=someip-bus
SUBNET=172.30.0.0/24
IMG="${IMG:-someip-base:3.6.4}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-$HERE/out}"
DUR="${DUR:-25}"

# A rota multicast precisa existir ANTES de o vsomeip subir, senao ele ja nasce
# com o SD desligado e so reavalia no proximo evento de netlink.
MC_ROUTE="ip route add 224.0.0.0/4 dev eth0 2>/dev/null"

mkdir -p "$OUT"
cleanup() {
    docker rm -f someip-service someip-client >/dev/null 2>&1
    docker network rm $NET >/dev/null 2>&1
}
cleanup
trap cleanup EXIT

docker network create --subnet $SUBNET $NET >/dev/null || exit 1
echo "[rede] $NET ($SUBNET)"

# O servico captura o proprio barramento. Capturar de um container separado NAO
# funciona para multicast: a bridge so entrega a quem entrou no grupo (IGMP),
# e um monitor passivo nao entra. Isso tambem e requisito de arquitetura: o
# monitor da bancada tera de entrar nos grupos ou usar espelhamento.
docker run -d --name someip-service --network $NET --ip 172.30.0.11 \
    --cap-add NET_ADMIN --cap-add NET_RAW -v "$HERE":/cfg -v "$OUT":/out "$IMG" \
    sh -c "$MC_ROUTE; tcpdump -i eth0 -w /out/sd-smoke.pcap -U 2>/dev/null &
           sleep 2
           VSOMEIP_CONFIGURATION=/cfg/service.json \
           VSOMEIP_APPLICATION_NAME=service-sample \
           /src/vsomeip/build/examples/notify-sample" >/dev/null
echo "[servico] 172.30.0.11  oferece 0x1234/0x5678"
sleep 6

docker run -d --name someip-client --network $NET --ip 172.30.0.12 \
    --cap-add NET_ADMIN -v "$HERE":/cfg "$IMG" \
    sh -c "$MC_ROUTE
           VSOMEIP_CONFIGURATION=/cfg/client.json \
           VSOMEIP_APPLICATION_NAME=client-sample \
           /src/vsomeip/build/examples/subscribe-sample" >/dev/null
echo "[cliente]  172.30.0.12  descobre e assina"

echo "[coletando ${DUR}s]"
sleep "$DUR"
docker exec someip-service sh -c "pkill tcpdump" >/dev/null 2>&1
sleep 1

echo
echo "===== netlink do vsomeip ====="
echo "  (mc=1 significa que a rota multicast foi vista; com mc=0 o SD nao sobe)"
docker logs someip-service 2>&1 | grep -E "netlink:" | tail -2 | sed 's/^/  /'

echo
echo "===== SOME/IP-SD capturado ====="
docker exec someip-service sh -c \
    "tcpdump -r /out/sd-smoke.pcap -nn 'udp port 30490' 2>/dev/null | head -3
     echo \"TOTAL: \$(tcpdump -r /out/sd-smoke.pcap -nn 'udp port 30490' 2>/dev/null | wc -l)\"" \
    2>/dev/null | sed 's/^/  /'

echo
echo "===== cliente recebeu? ====="
docker logs someip-client 2>&1 | grep -i "received a notification" | tail -3 | sed 's/^/  /'

echo
echo "  PCAP: $OUT/sd-smoke.pcap"
