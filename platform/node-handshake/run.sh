#!/usr/bin/env bash
# Fase 0, degrau final: dois nos NOSSOS, escritos com a libecu, no lugar dos
# exemplos do vsomeip.
#
# O platform/sd-smoke provou que o Service Discovery funciona entre containers,
# usando os binarios de exemplo. Este prova que funciona com a nossa fachada e
# com a configuracao GERADA da matriz de comunicacao, que e o criterio de pronto
# da Fase 0.
#
# PONTO CRITICO, herdado do sd-smoke: cada no precisa de uma ROTA MULTICAST. O
# Docker nao cria rota para 224.0.0.0/4, e o vsomeip so liga o Service Discovery
# depois de ver uma. Sem ela a oferta entra em laco silencioso de
# `pending_sd_offers`, sem erro nem aviso. Ver platform/sd-smoke/README.md.
#
# Pre-requisito: os identificadores e a config vem do gerador, nao da mao.
#     python tools/gen_config.py --out build/cfg
set -u

NET=someip-bus
SUBNET=172.30.0.0/24
IMG="${IMG:-someip-base:3.6.4}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${OUT:-$HERE/out}"
DUR="${DUR:-20}"

CFG="$ROOT/build/cfg"
if [ ! -f "$CFG/CommMatrix.hpp" ] || [ ! -f "$CFG/vsomeip-gnss_ecu.json" ]; then
    echo "ERRO: falta a saida do gerador. Rode primeiro:"
    echo "    python tools/gen_config.py --out build/cfg"
    exit 1
fi

# A rota precisa existir ANTES de o vsomeip subir, senao ele ja nasce com o SD
# desligado e so reavalia no proximo evento de netlink.
MC_ROUTE="ip route add 224.0.0.0/4 dev eth0 2>/dev/null"

mkdir -p "$OUT"
cleanup() {
    docker rm -f ecu-gnss ecu-hmi >/dev/null 2>&1
    docker network rm $NET >/dev/null 2>&1
}
cleanup
trap cleanup EXIT

# ---------------------------------------------------------------- 1. construir
# A compilacao roda como root dentro do container, entao os .o e os binarios
# nascem de root e o usuario do host nao consegue apagar depois. Limpar aqui
# dentro resolve, e de quebra garante build do zero.
echo "[build] compilando a libecu e os dois nos dentro da imagem"
docker run --rm -v "$ROOT":/w -w /w "$IMG" sh -c '
    set -e
    rm -rf build/obj build/bin
    mkdir -p build/obj build/bin
    for f in EcuErrorDomain Skeleton Proxy; do
        g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -c -o build/obj/$f.o src/libecu/$f.cpp
    done
    g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -o build/bin/gnss_ecu src/nodes/gnss_ecu.cpp build/obj/*.o -lvsomeip3 -pthread
    g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -o build/bin/hmi_ecu  src/nodes/hmi_ecu.cpp  build/obj/*.o -lvsomeip3 -pthread
' || { echo "ERRO: a compilacao falhou"; exit 1; }
echo "[build] ok"

# ---------------------------------------------------------------- 2. rede
docker network create --subnet $SUBNET $NET >/dev/null || exit 1
echo "[rede] $NET ($SUBNET)"

# ---------------------------------------------------------------- 3. provedor
# Captura no proprio container do provedor: um monitor passivo em container
# separado NAO ve o multicast, porque a bridge so entrega a quem entrou no
# grupo por IGMP.
docker run -d --name ecu-gnss --network $NET --ip 172.30.0.11 \
    --cap-add NET_ADMIN --cap-add NET_RAW \
    -v "$ROOT":/w -w /w -v "$OUT":/out "$IMG" \
    sh -c "$MC_ROUTE
           tcpdump -i eth0 -w /out/node-handshake.pcap -U 2>/dev/null &
           sleep 2
           VSOMEIP_CONFIGURATION=/w/build/cfg/vsomeip-gnss_ecu.json \
           VSOMEIP_APPLICATION_NAME=service-sample \
           ./build/bin/gnss_ecu" >/dev/null
echo "[gnss_ecu] 172.30.0.11  oferece e publica"
sleep 6

# ---------------------------------------------------------------- 4. consumidor
docker run -d --name ecu-hmi --network $NET --ip 172.30.0.12 \
    --cap-add NET_ADMIN \
    -v "$ROOT":/w -w /w "$IMG" \
    sh -c "$MC_ROUTE
           VSOMEIP_CONFIGURATION=/w/build/cfg/vsomeip-hmi_ecu.json \
           VSOMEIP_APPLICATION_NAME=client-sample \
           ./build/bin/hmi_ecu" >/dev/null
echo "[hmi_ecu]  172.30.0.12  descobre e assina"

echo "[coletando ${DUR}s]"
sleep "$DUR"
docker exec ecu-gnss sh -c "pkill tcpdump" >/dev/null 2>&1
sleep 1

# ---------------------------------------------------------------- 5. veredito
echo
echo "===== netlink: a rota multicast foi vista? ====="
echo "  (mc=1 e o sinal; com mc=0 o SD nunca sobe e nada abaixo funciona)"
docker logs ecu-gnss 2>&1 | grep -E "netlink:" | tail -2 | sed 's/^/  /'

echo
echo "===== provedor ====="
docker logs ecu-gnss 2>&1 | grep -E "^\[gnss_ecu\]" | head -6 | sed 's/^/  /'
echo "  ...  enviados: $(docker logs ecu-gnss 2>&1 | grep -c 'Position #')"

echo
echo "===== consumidor ====="
# As palavras procuradas abaixo sao a saida dos nos, que e em ingles como todo
# o codigo. Este script e documentacao de bancada, por isso fica em portugues.
docker logs ecu-hmi 2>&1 | grep -E "^\[hmi_ecu\]" | head -8 | sed 's/^/  /'
RECEBIDAS=$(docker logs ecu-hmi 2>&1 | grep -c 'notification [0-9]')
echo "  ...  recebidas: $RECEBIDAS"

echo
echo "===== SOME/IP no barramento ====="
# `-T someip` forca o dissector. Na porta do servico o tcpdump nao adivinha
# SOME/IP sozinho, so na 30490 do Service Discovery, e mostraria apenas
# "UDP, length 24". Com o dissector os campos ficam visiveis, e e assim que se
# confere que service e event batem com a matriz de comunicacao.
docker exec ecu-gnss sh -c \
    "echo \"  SD (porta 30490): \$(tcpdump -r /out/node-handshake.pcap -nn 'udp port 30490' 2>/dev/null | wc -l) pacotes\"
     echo \"  servico (porta 30509): \$(tcpdump -r /out/node-handshake.pcap -nn 'udp port 30509' 2>/dev/null | wc -l) pacotes\"
     echo \"  decodificado pelo dissector:\"
     tcpdump -r /out/node-handshake.pcap -nn -T someip 'udp port 30509' 2>/dev/null \
         | grep SOMEIP | head -2 | sed 's/^/    /'" 2>/dev/null

echo
if [ "$RECEBIDAS" -gt 0 ]; then
    echo "PASSOU: os dois nos nossos trocaram $RECEBIDAS notificacoes por SOME/IP."
else
    echo "FALHOU: nenhuma notificacao chegou. Veja o netlink acima e os logs."
fi
echo "  PCAP: $OUT/node-handshake.pcap"
