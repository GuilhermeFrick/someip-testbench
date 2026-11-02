#!/usr/bin/env bash
# Rodada viva: as 9 ECUs do barramento do Kim e um injetor de cenário adversarial
# no MESMO barramento, capturado na bridge.
#
# O injetor (src/nodes/injector_ecu.cpp) é uma ECU com IP próprio que só faz o que
# a pilha SOME/IP permite. Cada cenário entra na ordem em que o atacante entra no
# PCAP do dataset (tools/attack_timeline.py, platform/dup-offer):
#
#   dos    injetor no ar ANTES da VDE: as vítimas o encontram primeiro, abrem TCP
#          com ele, e a VDE só oferece ~4 s depois. IP .11, como no dataset.
#   mitm_single  barramento sobe normalmente; o injetor entra ~3 s depois da VDE,
#          anuncia a cada 10 ms e manda à ADAS um StopOffer em nome da VDE a cada
#          ~2,5 s. Publica o sinal falso do atacante do dataset, extraído do PCAP
#          (tools/extract_attacker_payloads.py). IP .13.
#   fuzzy  barramento sobe normalmente; o injetor entra depois. IP .17.
#
# O MAC do injetor é FIXADO, porque é o rótulo: o IP de origem pode ser forjado (o
# MITM multi do dataset forja o da VDE), o MAC do container não.
#
# Critério de aceite da fase A (docs/PLANO.md): tools/attack_flows.py sobre esta
# captura produz a mesma assinatura que sobre o PCAP do dataset.
#
# Uso, a partir da raiz do repo:
#     SCENARIO=dos   bash platform/scenario-live/run.sh
#     SCENARIO=fuzzy DUR=120 bash platform/scenario-live/run.sh
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT" || exit 1

SCENARIO="${SCENARIO:-}"
IMG="${IMG:-someip-base:3.5.6}"
NET="${NET:-someip-scn}"
SUBNET="${SUBNET:-172.28.0.0/24}"
DUR="${DUR:-90}"
PAYLOAD="${PAYLOAD:-counter}"
OUT="${OUT:-$HERE/out-$SCENARIO}"
WORK="build/scn"
CFG="$WORK/cfg"
PREFIXO="$(echo "$SUBNET" | cut -d/ -f1 | cut -d. -f1-3)."

case "$SCENARIO" in
    dos)         INJ_OCTETO=11 ;;
    mitm_single) INJ_OCTETO=13 ;;
    fuzzy)       INJ_OCTETO=17 ;;
    *) echo "ERRO: SCENARIO deve ser dos, mitm_single ou fuzzy"; exit 2 ;;
esac
INJ_IP="${PREFIXO}${INJ_OCTETO}"
INJ_MAC="$(echo "$INJ_IP" | awk -F. '{printf "02:42:%02x:%02x:%02x:%02x", $1, $2, $3, $4}')"

NOMES=""
limpa() {
    [ -n "$NOMES" ] && docker rm -f $NOMES >/dev/null 2>&1
    docker rm -f scn-cap scn-injector >/dev/null 2>&1
    docker network rm "$NET" >/dev/null 2>&1
}
limpa
trap limpa EXIT
mkdir -p "$OUT" "$WORK" build/cfg
rm -f "$OUT"/*

# ------------------------------------------------------- 1. matriz e configuração
python3 tools/remap_subnet.py --matrix platform/comm_matrix_kim.yaml --subnet "$SUBNET" \
    --out "$WORK/matriz.yaml" >/dev/null || exit 1
python3 tools/gen_config.py --matrix "$WORK/matriz.yaml" --out "$CFG" >/dev/null || exit 1
cp "$CFG/CommMatrixTable.hpp" build/cfg/CommMatrixTable.hpp || exit 1
python3 tools/gen_injector_config.py --cfg "$CFG" --scenario "$SCENARIO" --ip "$INJ_IP" \
    --out "$CFG/vsomeip-injector.json" || exit 1

# O sinal falso do MITM single vem do próprio atacante do dataset, pelo MAC dele.
DATASET="${DATASET:-$HOME/dataset/data/pcap}"
BANCO="build/cfg/attacker-mitm_single.bin"
if [ "$SCENARIO" = "mitm_single" ] && [ ! -f "$BANCO" ]; then
    python3 tools/extract_attacker_payloads.py --pcap "$DATASET/mitm_single_attacker.pcap" \
        --mac 02:42:ac:12:00:0d --service 0x1001 --event 0x0001 --out "$BANCO" || exit 1
fi

read -r PROVEDORES CONSUMIDORES <<EOF2
$(python3 - "$WORK/matriz.yaml" <<'PY'
import sys, yaml
m = yaml.safe_load(open(sys.argv[1], encoding="utf-8"))
p = [n for n, d in m["nodes"].items() if d.get("provides")]
c = [n for n, d in m["nodes"].items() if not d.get("provides")]
print(",".join(p), ",".join(c))
PY
)
EOF2
PROVEDORES="${PROVEDORES//,/ }"
CONSUMIDORES="${CONSUMIDORES//,/ }"
echo "[matriz] provedores: $PROVEDORES | consumidores: $CONSUMIDORES"

# -------------------------------------------------------------- 2. compilação
docker run --rm -v "$ROOT":/w -w /w "$IMG" sh -c '
    set -e
    rm -rf build/obj build/bin
    mkdir -p build/obj build/bin
    for f in EcuErrorDomain Skeleton Proxy NodeRuntime; do
        g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -O2 -c -o build/obj/$f.o src/libecu/$f.cpp
    done
    g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -O2 -o build/bin/generic_ecu  src/nodes/generic_ecu.cpp  build/obj/*.o -lvsomeip3 -pthread
    g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -O2 -o build/bin/injector_ecu src/nodes/injector_ecu.cpp build/obj/*.o -lvsomeip3 -pthread
' || { echo "ERRO: compilação"; exit 1; }
echo "[build] ok"

# ------------------------------------------------------------ 3. rede e captura
docker network create --subnet "$SUBNET" "$NET" >/dev/null || exit 1
BRIDGE="br-$(docker network inspect "$NET" --format '{{.Id}}' | cut -c1-12)"
docker run -d --name scn-cap --network host --cap-add NET_ADMIN --cap-add NET_RAW \
    -v "$OUT":/out "$IMG" sh -c "exec tcpdump -i $BRIDGE -w /out/scn.pcap -U -s 0" >/dev/null || exit 1
sleep 2
echo "[captura] $BRIDGE -> $OUT/scn.pcap"

# ------------------------------------------------------------------- 4. nós
ROTA="ip route add 224.0.0.0/4 dev eth0 2>/dev/null"
T0=$(date +%s.%N)
marca() { awk -v a="$(date +%s.%N)" -v b="$T0" -v m="$1" 'BEGIN { printf "%8.3f  %s\n", a - b, m }' >> "$OUT/linha-do-tempo.txt"; }

sobe_no() {
    local NO="$1"
    local IP
    IP="$(grep -A3 "^  $NO:" "$WORK/matriz.yaml" | grep -m1 'ip:' | sed 's/.*ip:[[:space:]]*//' | tr -d '\r')"
    NOMES="$NOMES ecu-$NO"
    docker run -d --name "ecu-$NO" --network "$NET" --ip "$IP" --cap-add NET_ADMIN \
        -v "$ROOT":/w -w /w "$IMG" \
        sh -c "$ROTA
               export VSOMEIP_CONFIGURATION=/w/$CFG/vsomeip-$NO.json
               export ECU_NODE=$NO ECU_PAYLOAD=$PAYLOAD ECU_REPORT_SECONDS=15
               exec ./build/bin/generic_ecu" >/dev/null || { echo "ERRO: $NO"; exit 1; }
    marca "sobe $NO ($IP)"
}
sobe_injetor() {
    docker run -d --name scn-injector --network "$NET" --ip "$INJ_IP" --mac-address "$INJ_MAC" \
        --cap-add NET_ADMIN --cap-add NET_RAW \
        -v "$ROOT":/w -w /w "$IMG" \
        sh -c "$ROTA
               export VSOMEIP_CONFIGURATION=/w/$CFG/vsomeip-injector.json
               export INJ_SCENARIO=$SCENARIO INJ_REPORT_SECONDS=15 INJ_LOCAL_IP=$INJ_IP
               exec ./build/bin/injector_ecu" >/dev/null || { echo "ERRO: injetor"; exit 1; }
    marca "sobe injetor $SCENARIO ($INJ_IP, MAC $INJ_MAC)"
    echo "[injetor] $SCENARIO em $INJ_IP, MAC $INJ_MAC"
}

if [ "$SCENARIO" = "dos" ]; then
    sobe_injetor
    sleep 2
    for n in $CONSUMIDORES; do sobe_no "$n"; done
    sleep 4
    for n in $PROVEDORES; do sobe_no "$n"; done
elif [ "$SCENARIO" = "mitm_single" ]; then
    # No dataset a VDE oferece em 3,7 s e o .13 em 6,6 s.
    for n in $PROVEDORES; do sobe_no "$n"; done
    sleep 1
    for n in $CONSUMIDORES; do sobe_no "$n"; done
    sleep 3
    sobe_injetor
else
    for n in $PROVEDORES; do sobe_no "$n"; done
    sleep 1
    for n in $CONSUMIDORES; do sobe_no "$n"; done
    sleep 10
    sobe_injetor
fi
echo "[coletando ${DUR}s]"
sleep "$DUR"

# ---------------------------------------------------------------- 5. encerrar
docker kill --signal=SIGTERM scn-injector >/dev/null 2>&1
for n in $PROVEDORES $CONSUMIDORES; do docker kill --signal=SIGTERM "ecu-$n" >/dev/null 2>&1; done
sleep 3
docker kill --signal=SIGINT scn-cap >/dev/null 2>&1
sleep 2
docker logs scn-injector > "$OUT/log-injector.txt" 2>&1
for n in $PROVEDORES $CONSUMIDORES; do docker logs "ecu-$n" > "$OUT/log-$n.txt" 2>&1; done
echo "$INJ_MAC" > "$OUT/injector.mac"

# ---------------------------------------------------------------- 6. veredito
echo
echo "===== injetor ====="
grep -E "^\[injector\]" "$OUT/log-injector.txt" | tail -4 | sed 's/^/  /'
echo
echo "===== o que cada ECU recebeu ====="
for n in $CONSUMIDORES; do
    printf '  %-8s ' "$n"
    grep -E "^\[$n\] 0x[0-9A-Fa-f]+/0x[0-9A-Fa-f]+: [0-9]+ recebidos" "$OUT/log-$n.txt" | tail -3 | sed -E "s/^\[$n\] //" | tr '\n' ' '
    echo
done
echo
echo "===== o barramento, pela regra do dataset ====="
python3 tools/attack_flows.py "$OUT/scn.pcap" --prefix "$PREFIXO"
echo
echo "===== ritmo das ofertas ====="
python3 tools/offer_cadence.py "$OUT/scn.pcap" | head -8
echo
echo "PCAP: $OUT/scn.pcap   linha do tempo: $OUT/linha-do-tempo.txt"
