#!/usr/bin/env bash
# A cadeia inteira no barramento: dois nós conversando e o sensor escutando.
#
# O platform/node-handshake provou que os nós trocam SOME/IP. Este prova que o
# ids_sensor CAPTURA esse tráfego no barramento, classifica cada quadro e reporta
# ao IdsM -- com os artefatos do treino de verdade, não com um modelo de prova.
#
# PONTO CRÍTICO, e é o motivo de o nó não ser trivial: um monitor passivo em
# container separado NÃO vê o multicast. A bridge do Docker só entrega a quem
# entrou no grupo por IGMP. O nó entra nos grupos que a matriz declara, antes de
# capturar; sem isso ele pega o tráfego unicast e ZERO pacote de Service
# Discovery, sem erro nenhum. Ver platform/sd-smoke/README.md.
#
# Pré-requisitos:
#     python3 tools/gen_config.py --out build/cfg
#     bash tools/build_sensor_artefacts.sh <dir-dos-artefatos-do-treino>
#
# Uso, a partir da raiz do repo:
#     bash platform/sensor-live/run.sh
set -u

NET=someip-bus
SUBNET=172.30.0.0/24
IMG="${IMG:-someip-base:3.5.6}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${OUT:-$HERE/out}"
DUR="${DUR:-25}"

CFG="$ROOT/build/cfg"
for f in CommMatrix.hpp vsomeip-gnss_ecu.json idsm-events.txt; do
    if [ ! -f "$CFG/$f" ]; then
        echo "ERRO: falta $CFG/$f. Rode primeiro:"
        echo "    python3 tools/gen_config.py --out build/cfg"
        exit 1
    fi
done
for f in bytemodels.bin minmax.bin tables.bin; do
    if [ ! -f "$CFG/$f" ]; then
        echo "ERRO: falta $CFG/$f. Converta os artefatos do treino:"
        echo "    bash tools/build_sensor_artefacts.sh <dir-dos-artefatos>"
        exit 1
    fi
done

MC_ROUTE="ip route add 224.0.0.0/4 dev eth0 2>/dev/null"

mkdir -p "$OUT"
rm -f "$OUT/security-events.txt"
cleanup() {
    docker rm -f ecu-gnss ecu-hmi ids-sensor >/dev/null 2>&1
    docker network rm $NET >/dev/null 2>&1
}
cleanup
trap cleanup EXIT

# ---------------------------------------------------------------- 1. construir
# Compila como root dentro do container, entao os artefatos de build nascem de
# root; limpar aqui dentro resolve e garante build do zero.
echo "[build] compilando os nos e o sensor dentro da imagem"
docker run --rm -v "$ROOT":/w -w /w "$IMG" sh -c '
    set -e
    rm -rf build/obj build/bin
    mkdir -p build/obj build/bin
    for f in EcuErrorDomain Skeleton Proxy; do
        g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -O2 -c -o build/obj/$f.o src/libecu/$f.cpp
    done
    g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -O2 -o build/bin/gnss_ecu src/nodes/gnss_ecu.cpp build/obj/*.o -lvsomeip3 -pthread
    g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -O2 -o build/bin/hmi_ecu  src/nodes/hmi_ecu.cpp  build/obj/*.o -lvsomeip3 -pthread
    g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -O2 -o build/bin/ids_sensor_node \
        src/nodes/ids_sensor_node.cpp \
        src/ids_sensor/Sensor.cpp src/ids_sensor/Classifier.cpp src/ids_sensor/Normalizer.cpp \
        src/ids_sensor/SomeipParser.cpp src/ids_sensor/ByteModel.cpp src/ids_sensor/FeatureStreamer.cpp \
        src/ids_sensor/PacketSource.cpp src/ids_sensor/SecurityEventDispatcher.cpp \
        src/ids_sensor/IdsSensorErrorDomain.cpp src/util/UtilErrorDomain.cpp \
        src/ara/idsm/EventReporter.cpp src/ara/core/InstanceSpecifier.cpp src/ara/core/CoreErrorDomain.cpp \
        -lpcap
' || { echo "ERRO: a compilacao falhou"; exit 1; }
echo "[build] ok"

# ---------------------------------------------------------------- 2. rede
docker network create --subnet $SUBNET $NET >/dev/null || exit 1
echo "[rede] $NET ($SUBNET)"

# ---------------------------------------------------------------- 3. sensor
# Sobe PRIMEIRO, para já estar nos grupos multicast quando o provedor anunciar.
# NET_ADMIN pela rota multicast, NET_RAW pela captura.
docker run -d --name ids-sensor --network $NET --ip 172.30.0.20 \
    --cap-add NET_ADMIN --cap-add NET_RAW \
    -v "$ROOT":/w -w /w -v "$OUT":/out \
    -e ARA_IDSM_CONFIGURATION=/w/build/cfg/idsm-events.txt \
    -e ARA_IDSM_SINK=/out/security-events.txt \
    -e IDS_INTERFACE=eth0 \
    -e IDS_LOCAL_IP=172.30.0.20 \
    -e IDS_ARTEFACTS=/w/build/cfg \
    "$IMG" sh -c "$MC_ROUTE
                  exec ./build/bin/ids_sensor_node" >/dev/null
echo "[ids_sensor] 172.30.0.20  entra nos grupos e captura"
sleep 4

# ---------------------------------------------------------------- 4. provedor
docker run -d --name ecu-gnss --network $NET --ip 172.30.0.11 \
    --cap-add NET_ADMIN \
    -v "$ROOT":/w -w /w "$IMG" \
    sh -c "$MC_ROUTE
           VSOMEIP_CONFIGURATION=/w/build/cfg/vsomeip-gnss_ecu.json \
           VSOMEIP_APPLICATION_NAME=service-sample \
           ./build/bin/gnss_ecu" >/dev/null
echo "[gnss_ecu]   172.30.0.11  oferece e publica"
sleep 6

# ---------------------------------------------------------------- 5. consumidor
docker run -d --name ecu-hmi --network $NET --ip 172.30.0.12 \
    --cap-add NET_ADMIN \
    -v "$ROOT":/w -w /w "$IMG" \
    sh -c "$MC_ROUTE
           VSOMEIP_CONFIGURATION=/w/build/cfg/vsomeip-hmi_ecu.json \
           VSOMEIP_APPLICATION_NAME=client-sample \
           ./build/bin/hmi_ecu" >/dev/null
echo "[hmi_ecu]    172.30.0.12  descobre e assina"

echo "[coletando ${DUR}s]"
sleep "$DUR"

# Parada limpa: o sensor imprime o resumo no SIGINT, e é esse resumo o veredito.
#
# ⚠️ O `exec` lá em cima é o que faz isso funcionar. Sem ele o PID 1 do container
# é o `sh`, o sinal vai para o shell e NÃO é repassado ao filho: o nó morre sem
# imprimir nada, e o veredito lê zero pacote mesmo tendo o sensor trabalhado.
# Foi exatamente o que aconteceu na primeira execução deste script.
docker kill --signal=SIGINT ids-sensor >/dev/null 2>&1
sleep 2

# ---------------------------------------------------------------- 6. veredito
echo
echo "===== o sensor entrou nos grupos multicast? ====="
echo "  (sem isso ele NAO ve Service Discovery, e nao reclama)"
docker logs ids-sensor 2>&1 | grep -E "joined|WARNING" | sed 's/^/  /'

echo
echo "===== motor de inferencia ====="
docker logs ids-sensor 2>&1 | grep -E "inference engine" | sed 's/^/  /'

echo
echo "===== o que o sensor viu ====="
docker logs ids-sensor 2>&1 | grep -E "^\[ids_sensor\] (packets|dropped|security|per packet)" | sed 's/^/  /'

echo
echo "===== os dois nos conversaram? ====="
ENVIADAS=$(docker logs ecu-gnss 2>&1 | grep -c 'Position #')
RECEBIDAS=$(docker logs ecu-hmi 2>&1 | grep -c 'notification [0-9]')
echo "  gnss_ecu enviou:  $ENVIADAS"
echo "  hmi_ecu  recebeu: $RECEBIDAS"

echo
echo "===== eventos de seguranca reportados ao IdsM ====="
if [ -s "$OUT/security-events.txt" ]; then
    echo "  $(wc -l < "$OUT/security-events.txt") linhas em $OUT/security-events.txt"
    # A contagem por classe sozinha engana: num barramento real os eventos vem de
    # origens diferentes por motivos diferentes, e e a separacao que diagnostica.
    if command -v python3 >/dev/null 2>&1; then
        python3 "$HERE/resumo_eventos.py" "$OUT/security-events.txt"
    else
        sort "$OUT/security-events.txt" | sed 's/path=//' | awk '{print $2}' | sort | uniq -c | sed 's/^/    /'
    fi
else
    echo "  nenhum. Com trafego BENIGNO e o esperado: a classe Normal nao levanta evento."
fi

echo
echo "===== leia os tempos com cuidado ====="
echo "  Sao poucas dezenas de pacotes, o estagio 'decide' mede o sink da bancada"
echo "  (um arquivo aberto e fechado POR EVENTO) e ha cinco leituras de relogio"
echo "  por quadro na conta. Este degrau prova FUNCAO, nao desempenho; o numero"
echo "  limpo e o do banco de latencia. Ver platform/sensor-live/README.md."

echo
PACOTES=$(docker logs ids-sensor 2>&1 | grep -oP '^\[ids_sensor\] packets\s+: \K[0-9]+' | tail -1)
PACOTES=${PACOTES:-0}
if [ "$RECEBIDAS" -gt 0 ] && [ "$PACOTES" -gt 0 ]; then
    echo "PASSOU: os nos trocaram $RECEBIDAS notificacoes e o sensor classificou $PACOTES pacotes do barramento."
else
    echo "FALHOU: notificacoes=$RECEBIDAS, pacotes classificados=$PACOTES. Veja os logs acima."
fi
