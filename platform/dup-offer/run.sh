#!/usr/bin/env bash
# Fase A0, risco 1: o vsomeip 3.6.4 aceita DUAS ECUs oferecendo o mesmo serviço?
#
# Os quatro ataques do dataset do Kim dependem disso. No DoS e nos dois MITM o
# atacante anuncia o serviço 0x1001, que é da VDE, e as vítimas assinam e abrem
# TCP com ele (medido em tools/attack_flows.py). Se a nossa versão do vsomeip
# recusar ou ignorar a segunda oferta, nenhum atacante vivo reproduz o dataset, e
# isso tem de ser sabido antes de escrever uma linha de código de ataque.
#
# O teste é o começo do DoS sem a inundação:
#   - VDE verdadeira (.10) oferece 0x1001;
#   - ADAS (.2) e CLU (.3) sobem e assinam a VDE;
#   - depois de CLONE_AT segundos sobe um CLONE da VDE com IP .11: o mesmo binário,
#     a mesma entrada da matriz, só a configuração vsomeip com outro unicast.
#
# O veredito é o que o barramento mostra, lido por tools/attack_flows.py com a regra
# do dataset (origem fora das 9 ECUs = atacante): o clone ofereceu? alguma vítima
# assinou nele? quem abriu o TCP com ele? chegaram notificações dele às vítimas?
#
# Uso, a partir da raiz do repo:
#     bash platform/dup-offer/run.sh
#     SUBNET=172.28.0.0/24 DUR=60 CLONE_AT=15 bash platform/dup-offer/run.sh
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT" || exit 1

IMG="${IMG:-someip-base:3.6.4}"
NET="${NET:-someip-dup}"
SUBNET="${SUBNET:-172.28.0.0/24}"
DUR="${DUR:-60}"
CLONE_AT="${CLONE_AT:-15}"
CLONE_CYCLE_MS="${CLONE_CYCLE_MS:-20}"
OUT="${OUT:-$HERE/out}"
WORK="build/dup"
CFG="$WORK/cfg"
PREFIXO="$(echo "$SUBNET" | cut -d/ -f1 | cut -d. -f1-3)."

NOMES="dup-cap ecu-node_10 ecu-node_2 ecu-node_3 ecu-clone"
limpa() {
    docker rm -f $NOMES >/dev/null 2>&1
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

# O clone é a VDE com outro endereço. Só o unicast muda: mesmo serviço, mesma
# instância, mesmas portas. É exatamente o que o .11 anuncia no PCAP do dataset.
sed "s/\"${PREFIXO//./\\.}10\"/\"${PREFIXO}11\"/" "$CFG/vsomeip-node_10.json" > "$CFG/vsomeip-clone.json"
if ! grep -q "\"${PREFIXO}11\"" "$CFG/vsomeip-clone.json"; then
    echo "ERRO: não achei o unicast ${PREFIXO}10 em vsomeip-node_10.json para trocar"
    exit 1
fi
# O ritmo da oferta é parte do ataque: no dataset o DoS anuncia a cada 20 ms e o
# MITM single a cada 10 ms, contra 2000 ms de uma ECU legítima (tools/offer_cadence.py).
# Com o ritmo normal, a primeira rodada deste degrau mostrou as vítimas assinando no
# clone mas mantendo o TCP com a VDE verdadeira.
python3 - "$CFG/vsomeip-clone.json" "${CLONE_CYCLE_MS}" <<'PY' || exit 1
import json, sys
caminho, ciclo = sys.argv[1], sys.argv[2]
d = json.load(open(caminho))
d["service-discovery"]["cyclic_offer_delay"] = ciclo
json.dump(d, open(caminho, "w"), indent=2)
PY
echo "[cfg] clone da VDE: unicast ${PREFIXO}11, oferta a cada ${CLONE_CYCLE_MS} ms"

# -------------------------------------------------------------- 2. compilação
docker run --rm -v "$ROOT":/w -w /w "$IMG" sh -c '
    set -e
    rm -rf build/obj build/bin
    mkdir -p build/obj build/bin
    for f in EcuErrorDomain Skeleton Proxy NodeRuntime; do
        g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -O2 -c -o build/obj/$f.o src/libecu/$f.cpp
    done
    g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -O2 -o build/bin/generic_ecu \
        src/nodes/generic_ecu.cpp build/obj/*.o -lvsomeip3 -pthread
' || { echo "ERRO: compilação"; exit 1; }
echo "[build] ok"

# ------------------------------------------------------------ 3. rede e captura
docker network create --subnet "$SUBNET" "$NET" >/dev/null || exit 1
BRIDGE="br-$(docker network inspect "$NET" --format '{{.Id}}' | cut -c1-12)"
docker run -d --name dup-cap --network host --cap-add NET_ADMIN --cap-add NET_RAW \
    -v "$OUT":/out "$IMG" sh -c "exec tcpdump -i $BRIDGE -w /out/dup.pcap -U -s 0" >/dev/null || exit 1
sleep 2

# ------------------------------------------------------------------- 4. nós
sobe() {  # nome  último-octeto  ECU_NODE  config
    docker run -d --name "ecu-$1" --network "$NET" --ip "${PREFIXO}$2" --cap-add NET_ADMIN \
        -v "$ROOT":/w -w /w "$IMG" \
        sh -c "ip route add 224.0.0.0/4 dev eth0 2>/dev/null
               export VSOMEIP_CONFIGURATION=/w/$CFG/$4
               export ECU_NODE=$3
               export ECU_PAYLOAD=counter
               export ECU_REPORT_SECONDS=10
               exec ./build/bin/generic_ecu" >/dev/null || { echo "ERRO: $1"; exit 1; }
    echo "[nó] $1 em ${PREFIXO}$2"
}
sobe node_10 10 node_10 vsomeip-node_10.json
sleep 2
sobe node_2 2 node_2 vsomeip-node_2.json
sobe node_3 3 node_3 vsomeip-node_3.json
echo "[espera] ${CLONE_AT}s para as vítimas assinarem a VDE verdadeira"
sleep "$CLONE_AT"
sobe clone 11 node_10 vsomeip-clone.json
RESTO=$((DUR - CLONE_AT))
echo "[coletando] mais ${RESTO}s com as duas ofertas no ar"
sleep "$RESTO"

# ---------------------------------------------------------------- 5. encerrar
for n in node_10 node_2 node_3 clone; do docker kill --signal=SIGTERM "ecu-$n" >/dev/null 2>&1; done
sleep 3
docker kill --signal=SIGINT dup-cap >/dev/null 2>&1
sleep 2
for n in node_10 node_2 node_3 clone; do docker logs "ecu-$n" > "$OUT/log-$n.txt" 2>&1; done

# ---------------------------------------------------------------- 6. veredito
echo
echo "===== o que cada nó disse ao encerrar ====="
for n in node_10 clone node_2 node_3; do
    echo "--- $n"
    grep -E "enviados|recebidos|encerrando" "$OUT/log-$n.txt" | tail -4 | sed 's/^/    /'
done
echo
echo "===== o barramento, pela regra do dataset (origem fora das 9 ECUs = atacante) ====="
python3 tools/attack_flows.py "$OUT/dup.pcap" --prefix "$PREFIXO"
echo
echo "PCAP: $OUT/dup.pcap   logs: $OUT/log-*.txt"
