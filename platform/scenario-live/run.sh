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
# SENSOR=1 põe o ids_sensor_node na rodada (fase B); IDS_FILTER é o filtro BPF dele,
# vazio por padrão porque o filtro faz parte da medição.
SENSOR="${SENSOR:-1}"
IDS_FILTER="${IDS_FILTER:-}"
# Fase D: cada injetor também fica na rede CTRL, que nunca é capturada, e recebe
# comandos nela (src/nodes/CtrlChannel.hpp, tools/inj_ctl.py). CTRL_PLAN é um roteiro
# "segundos:comando;segundos:comando", com o tempo contado de T0, por exemplo
#     CTRL_PLAN="25:off;40:on"
CTRL_NET="${CTRL_NET:-someip-ctrl}"
CTRL_SUBNET="${CTRL_SUBNET:-172.29.0.0/24}"
CTRL_PORT="${CTRL_PORT:-7000}"
CTRL_PLAN="${CTRL_PLAN:-}"
CTRL_PREFIXO="$(echo "$CTRL_SUBNET" | cut -d/ -f1 | cut -d. -f1-3)."
OUT="${OUT:-$HERE/out-$SCENARIO}"
WORK="build/scn"
CFG="$WORK/cfg"
PREFIXO="$(echo "$SUBNET" | cut -d/ -f1 | cut -d. -f1-3)."

# Injetores de cada cenário, como papel:último-octeto. O MITM multi tem dois, como no
# dataset: .14 assina a VDE e repassa por 0x100B, .15 se passa pela VDE.
case "$SCENARIO" in
    dos)         INJETORES="dos:11" ;;
    mitm_single) INJETORES="mitm_single:13" ;;
    mitm_multi)  INJETORES="mitm_relay:14 mitm_impostor:15" ;;
    fuzzy)       INJETORES="fuzzy:17" ;;
    none)        INJETORES="" ;;   # só o barramento benigno: mede o falso positivo
    *) echo "ERRO: SCENARIO deve ser dos, mitm_single, mitm_multi, fuzzy ou none"; exit 2 ;;
esac
ip_de()  { echo "${PREFIXO}${1##*:}"; }
mac_de() { echo "$(ip_de "$1")" | awk -F. '{printf "02:42:%02x:%02x:%02x:%02x", $1, $2, $3, $4}'; }

NOMES=""
limpa() {
    [ -n "$NOMES" ] && docker rm -f $NOMES >/dev/null 2>&1
    docker rm -f scn-cap scn-sensor $(docker ps -aq --filter name=scn-inj-) >/dev/null 2>&1
    docker network rm "$NET" "$CTRL_NET" >/dev/null 2>&1
}
limpa
trap limpa EXIT
mkdir -p "$OUT" "$WORK" build/cfg
rm -f "$OUT"/*

# ------------------------------------------------------- 1. matriz e configuração
python3 tools/remap_subnet.py --matrix platform/comm_matrix_kim.yaml --subnet "$SUBNET" \
    --out "$WORK/matriz.yaml" >/dev/null || exit 1

# O sensor é INSTRUMENTAÇÃO da bancada, não parte do barramento reproduzido: a
# matriz do Kim descreve só as 9 ECUs e continua assim. Ele entra só na matriz
# derivada desta rodada, com os security events que reporta ao IdsM, para o
# gerador escrever a estrutura IdsSensor e o idsm-events.txt.
if [ "$SENSOR" = "1" ]; then
    python3 - "$WORK/matriz.yaml" "${PREFIXO}20" <<'PY' || exit 1
import sys, yaml
caminho, ip = sys.argv[1], sys.argv[2]
m = yaml.safe_load(open(caminho, encoding="utf-8"))
m["nodes"]["ids_sensor"] = {"ip": ip, "app_id": 0x1400, "app_name": "ids-sensor", "role": "monitor",
                            "observes": list(m["services"])}
m["security_events"] = {"port_prefix": "ids_sensor/SecurityEvents", "events": [
    {"class": "Dos", "name": "DenialOfService"}, {"class": "Fuzzy", "name": "MalformedPayload"},
    {"class": "MitmSingle", "name": "ManInTheMiddleSingle"}, {"class": "MitmMulti", "name": "ManInTheMiddleMulti"}]}
with open(caminho, "w", encoding="utf-8") as fh:
    fh.write("# Derivada de platform/comm_matrix_kim.yaml para uma rodada viva: sub-rede\n"
             "# remapeada e o sensor da bancada acrescentado como instrumentação.\n")
    yaml.safe_dump(m, fh, sort_keys=False, allow_unicode=True)
PY
fi
python3 tools/gen_config.py --matrix "$WORK/matriz.yaml" --out "$CFG" >/dev/null || exit 1
# Os cabeçalhos desta matriz vão para uma raiz PRÓPRIA da rodada, que vem antes de "."
# na compilação (os nós incluem "build/cfg/CommMatrix*.hpp"). Copiá-los por cima de
# build/cfg quebrava o tests/test_comm_matrix.cpp, que espera os da matriz base.
RAIZ="$WORK/root"
mkdir -p "$RAIZ/build/cfg"
cp "$CFG/CommMatrixTable.hpp" "$CFG/CommMatrix.hpp" "$RAIZ/build/cfg/" || exit 1
N_APP=0
for INJ in $INJETORES; do
    PAPEL="${INJ%%:*}"
    N_APP=$((N_APP + 1))
    python3 tools/gen_injector_config.py --cfg "$CFG" --scenario "$PAPEL" --ip "$(ip_de "$INJ")" \
        --app-id "$((0x1EE0 + N_APP))" --out "$CFG/vsomeip-inj-$PAPEL.json" || exit 1
done

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
c = [n for n, d in m["nodes"].items() if not d.get("provides") and d.get("role") != "monitor"]
print(",".join(p), ",".join(c))
PY
)
EOF2
PROVEDORES="${PROVEDORES//,/ }"
CONSUMIDORES="${CONSUMIDORES//,/ }"
echo "[matriz] provedores: $PROVEDORES | consumidores: $CONSUMIDORES"

# -------------------------------------------------------------- 2. compilação
docker run --rm -v "$ROOT":/w -w /w -e RAIZ="$RAIZ" "$IMG" sh -c '
    set -e
    rm -rf build/obj build/bin
    mkdir -p build/obj build/bin
    for f in EcuErrorDomain Skeleton Proxy NodeRuntime; do
        g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -O2 -c -o build/obj/$f.o src/libecu/$f.cpp
    done
    g++ -std=c++14 -I$RAIZ -I. -Wall -Wextra -Wpedantic -O2 -o build/bin/generic_ecu  src/nodes/generic_ecu.cpp  build/obj/*.o -lvsomeip3 -pthread
    g++ -std=c++14 -I$RAIZ -I. -Wall -Wextra -Wpedantic -O2 -o build/bin/injector_ecu src/nodes/injector_ecu.cpp build/obj/*.o -lvsomeip3 -pthread
    if [ "'"$SENSOR"'" = "1" ]; then
        g++ -std=c++14 -I$RAIZ -I. -Wall -Wextra -Wpedantic -O2 -o build/bin/ids_sensor_node \
            src/nodes/ids_sensor_node.cpp \
            src/ids_sensor/Sensor.cpp src/ids_sensor/Classifier.cpp src/ids_sensor/Normalizer.cpp \
            src/ids_sensor/SomeipParser.cpp src/ids_sensor/ByteModel.cpp src/ids_sensor/FeatureStreamer.cpp \
            src/ids_sensor/PacketSource.cpp src/ids_sensor/SecurityEventDispatcher.cpp \
            src/ids_sensor/IdsSensorErrorDomain.cpp src/util/UtilErrorDomain.cpp \
            src/ara/idsm/EventReporter.cpp src/ara/core/InstanceSpecifier.cpp src/ara/core/CoreErrorDomain.cpp \
            -lpcap
    fi
' || { echo "ERRO: compilação"; exit 1; }
echo "[build] ok"

# ------------------------------------------------------------ 3. rede e captura
docker network create --subnet "$SUBNET" "$NET" >/dev/null || exit 1
BRIDGE="br-$(docker network inspect "$NET" --format '{{.Id}}' | cut -c1-12)"
# A rede de controle é OUTRA bridge, e a captura só escuta a do barramento.
docker network create --subnet "$CTRL_SUBNET" "$CTRL_NET" >/dev/null || exit 1
docker run -d --name scn-cap --network host --cap-add NET_ADMIN --cap-add NET_RAW \
    -v "$OUT":/out "$IMG" sh -c "exec tcpdump -i $BRIDGE -w /out/scn.pcap -U -s 0" >/dev/null || exit 1
sleep 2
echo "[captura] $BRIDGE -> $OUT/scn.pcap"

# O sensor escuta na INTERFACE DA BRIDGE, como a captura: um container comum, numa
# bridge que funciona como switch, não vê o TCP entre as outras ECUs. Num veículo o
# IDS fica numa porta espelhada do switch; aqui o equivalente é a bridge. Sobe antes
# das ECUs, para ver o barramento desde o primeiro quadro.
if [ "$SENSOR" = "1" ]; then
    for f in bytemodels.bin minmax.bin tables.bin; do
        [ -f "build/cfg/$f" ] || { echo "ERRO: falta build/cfg/$f (tools/build_sensor_artefacts.sh)"; exit 1; }
    done
    docker run -d --name scn-sensor --network host --cap-add NET_ADMIN --cap-add NET_RAW \
        -v "$ROOT":/w -w /w -v "$OUT":/out \
        -e ARA_IDSM_CONFIGURATION=/w/$CFG/idsm-events.txt \
        -e ARA_IDSM_SINK=/out/security-events.txt \
        -e IDS_INTERFACE="$BRIDGE" -e IDS_ARTEFACTS=/w/build/cfg -e IDS_FILTER="$IDS_FILTER" \
        "$IMG" sh -c "exec ./build/bin/ids_sensor_node" >/dev/null || { echo "ERRO: sensor"; exit 1; }
    sleep 2
    echo "[sensor] ids_sensor_node na $BRIDGE${IDS_FILTER:+, filtro \"$IDS_FILTER\"}"
fi

# ------------------------------------------------------------------- 4. nós
ROTA="ip route add 224.0.0.0/4 dev eth0 2>/dev/null"
T0=$(date +%s.%N)
# O sensor carimba os eventos com o horário do quadro (epoch); T0 permite pô-los na
# mesma régua da linha do tempo.
echo "T0_EPOCH=$T0" > "$OUT/linha-do-tempo.txt"
# LC_ALL=C: com o idioma do sistema em pt_BR o awk escreve "6,320", e a linha do tempo
# deixa de ser lida como número.
marca() { LC_ALL=C awk -v a="$(date +%s.%N)" -v b="$T0" -v m="$1" 'BEGIN { printf "%8.3f  %s\n", a - b, m }' >> "$OUT/linha-do-tempo.txt"; }

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
               export ECU_PAYLOAD_FILE=${PAYLOAD_FILE:-build/cfg/payloads.bin}
               exec ./build/bin/generic_ecu" >/dev/null || { echo "ERRO: $NO"; exit 1; }
    marca "sobe $NO ($IP)"
}
sobe_injetor() {  # papel:octeto
    local PAPEL="${1%%:*}"
    local IP MAC
    IP="$(ip_de "$1")"
    MAC="$(mac_de "$1")"
    local CIP="${CTRL_PREFIXO}${1##*:}"
    # create + connect + start: a interface de controle tem de existir quando o injetor
    # abre o canal. O barramento é a eth0 (a rota multicast vai nela); o controle, a eth1.
    # Com duas redes o Docker NÃO garante que o barramento seja a eth0: numa rodada do MITM
    # multi o .15 saiu com o SD inteiro na rede de controle e nunca achou o .14. A rota
    # multicast vai na interface que TEM o endereço do barramento, achada pelo IP.
    local ROTA_INJ="DEV=\$(ip -o -4 addr show | awk '{split(\$4, a, \"/\"); if (a[1] == \"$IP\") print \$2}')
               ip route add 224.0.0.0/4 dev \$DEV 2>/dev/null"
    docker create --name "scn-inj-$PAPEL" --network "$NET" --ip "$IP" --mac-address "$MAC" \
        --cap-add NET_ADMIN --cap-add NET_RAW \
        -v "$ROOT":/w -w /w "$IMG" \
        sh -c "$ROTA_INJ
               export VSOMEIP_CONFIGURATION=/w/$CFG/vsomeip-inj-$PAPEL.json
               export INJ_SCENARIO=$PAPEL INJ_REPORT_SECONDS=15 INJ_LOCAL_IP=$IP
               export INJ_CTRL_ADDR=$CIP:$CTRL_PORT
               exec ./build/bin/injector_ecu" >/dev/null || { echo "ERRO: injetor $PAPEL"; exit 1; }
    docker network connect --ip "$CIP" "$CTRL_NET" "scn-inj-$PAPEL" || { echo "ERRO: ctrl $PAPEL"; exit 1; }
    docker start "scn-inj-$PAPEL" >/dev/null || { echo "ERRO: start $PAPEL"; exit 1; }
    echo "$CIP:$CTRL_PORT $PAPEL" >> "$OUT/injector.ctrl"
    marca "sobe injetor $PAPEL ($IP, MAC $MAC, ctrl $CIP:$CTRL_PORT)"
    echo "$MAC $PAPEL" >> "$OUT/injector.mac"
    echo "[injetor] $PAPEL em $IP, MAC $MAC"
}

if [ "$SCENARIO" = "dos" ]; then
    sobe_injetor "dos:11"
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
    sobe_injetor "mitm_single:13"
elif [ "$SCENARIO" = "mitm_multi" ]; then
    # No dataset o .14 aparece em 0,0 s, as vítimas procuram entre 0,8 e 2,5 s, a VDE
    # oferece em 3,9 s e o .15 em 7,3 s.
    sobe_injetor "mitm_relay:14"
    sleep 1
    for n in $CONSUMIDORES; do sobe_no "$n"; done
    sleep 2
    for n in $PROVEDORES; do sobe_no "$n"; done
    sleep 3
    sobe_injetor "mitm_impostor:15"
elif [ "$SCENARIO" = "fuzzy" ]; then
    for n in $PROVEDORES; do sobe_no "$n"; done
    sleep 1
    for n in $CONSUMIDORES; do sobe_no "$n"; done
    sleep 10
    sobe_injetor "fuzzy:17"
else
    for n in $PROVEDORES; do sobe_no "$n"; done
    sleep 1
    for n in $CONSUMIDORES; do sobe_no "$n"; done
fi
touch "$OUT/injector.mac" "$OUT/injector.ctrl"
echo "[coletando ${DUR}s]"
FIM=$(( $(date +%s) + DUR ))
# Roteiro de controle: cada passo vai a todos os injetores da rodada, pela rede ctrl, e
# entra na linha do tempo com a resposta deles.
if [ -n "$CTRL_PLAN" ]; then
    IFS=';' read -r -a PASSOS <<< "$CTRL_PLAN"
    for PASSO in "${PASSOS[@]}"; do
        QUANDO="${PASSO%%:*}"
        CMD="${PASSO#*:}"
        ALVO_T=$(LC_ALL=C awk -v t0="$T0" -v q="$QUANDO" 'BEGIN { printf "%.3f", t0 + q }')
        AGORA=$(date +%s.%N)
        ESPERA=$(LC_ALL=C awk -v a="$ALVO_T" -v b="$AGORA" 'BEGIN { d = a - b; printf "%.3f", (d > 0 ? d : 0) }')
        sleep "$ESPERA"
        while read -r END PAPEL; do
            RESP="$(python3 tools/inj_ctl.py --to "$END" $CMD 2>&1 | sed -E 's/^[0-9.]+ //')"
            marca "ctrl $PAPEL $RESP"
            echo "[ctrl] $PAPEL: $RESP"
        done < "$OUT/injector.ctrl"
    done
fi
RESTA=$(( FIM - $(date +%s) ))
[ "$RESTA" -gt 0 ] && sleep "$RESTA"

# ---------------------------------------------------------------- 5. encerrar
for INJ in $INJETORES; do docker kill --signal=SIGTERM "scn-inj-${INJ%%:*}" >/dev/null 2>&1; done
for n in $PROVEDORES $CONSUMIDORES; do docker kill --signal=SIGTERM "ecu-$n" >/dev/null 2>&1; done
sleep 3
# O sensor imprime o resumo no SIGINT; é esse resumo que diz quantos quadros ele viu.
if [ "$SENSOR" = "1" ]; then
    docker kill --signal=SIGINT scn-sensor >/dev/null 2>&1
    # Espera ele sair: o resumo final só é escrito depois do laço de captura parar.
    timeout 20 docker wait scn-sensor >/dev/null 2>&1
fi
docker kill --signal=SIGINT scn-cap >/dev/null 2>&1
sleep 2
for INJ in $INJETORES; do docker logs "scn-inj-${INJ%%:*}" > "$OUT/log-inj-${INJ%%:*}.txt" 2>&1; done
for n in $PROVEDORES $CONSUMIDORES; do docker logs "ecu-$n" > "$OUT/log-$n.txt" 2>&1; done
[ "$SENSOR" = "1" ] && docker logs scn-sensor > "$OUT/log-sensor.txt" 2>&1

# Rótulo pelo MAC dos injetores, no formato do score_pcap.
python3 tools/label_by_mac.py --pcap "$OUT/scn.pcap" --macs "$OUT/injector.mac" --prefix "$PREFIXO" \
    --out "$OUT/scn.labels" > "$OUT/rotulo.txt" || exit 1

# ---------------------------------------------------------------- 6. veredito
echo
echo "===== injetores ====="
for INJ in $INJETORES; do
    grep -E "^\[injector\]" "$OUT/log-inj-${INJ%%:*}.txt" | tail -3 | sed "s/^/  ${INJ%%:*}: /"
done
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
echo "===== atividade dos injetores no barramento (janelas de 5 s) ====="
python3 tools/attack_activity.py --pcap "$OUT/scn.pcap" --macs "$OUT/injector.mac" \
    --timeline "$OUT/linha-do-tempo.txt" --ctrl-port "$CTRL_PORT" | sed 's/^/  /'
echo
echo "===== ritmo das ofertas ====="
python3 tools/offer_cadence.py "$OUT/scn.pcap" | head -8
echo
echo "===== rótulo pelo MAC ====="
sed 's/^/  /' "$OUT/rotulo.txt"
if [ "$SENSOR" = "1" ]; then
    echo
    echo "===== sensor ao vivo ====="
    grep -E "^\[ids_sensor\] (inference engine|packets|dropped|security events)" "$OUT/log-sensor.txt" | sed 's/^/  /'
    if [ -s "$OUT/security-events.txt" ]; then
        python3 tools/events_timeline.py --events "$OUT/security-events.txt" --timeline "$OUT/linha-do-tempo.txt" | sed 's/^/  /'
    else
        echo "  nenhum security event"
    fi
fi
echo
echo "PCAP: $OUT/scn.pcap   linha do tempo: $OUT/linha-do-tempo.txt"
