#!/usr/bin/env bash
# Paridade do sensor sobre o DATASET INTEIRO: 14 milhoes de pacotes reais.
#
# O teste sintetico (tests/test_parity_streamer.cpp, 23 pacotes) cobre os casos
# de borda que eu soube imaginar. Este cobre o que eu nao soube: roda os 7 PCAPs
# do dataset pelos DOIS lados e compara feature a feature.
#
# A cadeia exercitada e a MESMA que roda em producao:
#     PcapSource -> SomeipParser -> FeatureStreamer
# Nao existe caminho de codigo separado para o teste. E por isso que o
# PcapSource existe: sem ele, a paridade mediria um codigo que ninguem executa.
#
# A imagem e a deploy/Dockerfile.parity, SEM vsomeip: esta cadeia nao toca a
# pilha SOME/IP, e compilar o vsomeip custaria dez minutos por nada.
#
# Pre-requisitos:
#   - os 7 PCAPs do dataset (figshare, ver docs/), em $PCAP_DIR
#   - o repo de reproducao ao lado, em $REPRO, pelos modulos de referencia
#   - a imagem:  docker build -f deploy/Dockerfile.parity -t someip-parity:1 .
#
# Uso, a partir da raiz do repo:
#     bash platform/parity-dataset/run.sh
#     MAX_PACKETS=50000 bash platform/parity-dataset/run.sh    # ensaio rapido
set -u

IMG="${IMG:-someip-parity:1}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PCAP_DIR="${PCAP_DIR:-$HOME/dataset/data/pcap}"
REPRO="${REPRO:-$ROOT/../someip-ids-multiclass-contentext}"
OUT="${OUT:-$ROOT/build/parity-dataset}"
MAX_PACKETS="${MAX_PACKETS:-}"
LOG="$HERE/out/parity-dataset.log"

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "ERRO: imagem $IMG ausente. Construa antes:"
    echo "    docker build -f deploy/Dockerfile.parity -t someip-parity:1 ."
    exit 1
fi
if [ ! -d "$PCAP_DIR" ]; then
    echo "ERRO: $PCAP_DIR nao existe. Aponte PCAP_DIR para os 7 PCAPs do dataset."
    exit 1
fi
if [ ! -f "$REPRO/src/extract_ext.py" ]; then
    echo "ERRO: nao achei os modulos de referencia em $REPRO/src."
    echo "      Sem eles isto nao e paridade, e so a minha implementacao concordando consigo mesma."
    exit 1
fi

# $OUT e criado DENTRO do container, pelo gerador. Aqui so o diretorio do log.
mkdir -p "$HERE/out" || { echo "ERRO: nao consegui criar $HERE/out"; exit 1; }

LIMITE=""
if [ -n "$MAX_PACKETS" ]; then
    LIMITE="--max-packets $MAX_PACKETS"
    echo "[ensaio] limitado a $MAX_PACKETS pacotes por arquivo"
fi

# O binario compilado fica em /tmp dentro do container, mas os registros de
# referencia vao para a arvore do host montada em /w, e o container roda como
# root. Sem o chown no fim, build/ fica de root e o usuario do host nao consegue
# mais escrever ali: o proximo `mkdir -p` falha e o motivo nao e obvio.
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"

docker run --rm \
    -v "$ROOT":/w \
    -v "$REPRO":/repro:ro \
    -v "$PCAP_DIR":/pcap:ro \
    -w /w "$IMG" sh -c "
set -e
# Devolve a saida ao usuario do host, mesmo quando a comparacao falha: sem isto
# uma rodada vermelha deixaria a arvore intocavel.
trap 'chown -R $HOST_UID:$HOST_GID /w/build 2>/dev/null || true' EXIT

echo '[1/3] compilando a cadeia e a ferramenta de comparacao'
g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -O2 -o /tmp/parity_dataset \
    tests/parity_dataset.cpp \
    src/ids_sensor/PacketSource.cpp \
    src/ids_sensor/SomeipParser.cpp \
    src/ids_sensor/ByteModel.cpp \
    src/ids_sensor/FeatureStreamer.cpp \
    src/ids_sensor/IdsSensorErrorDomain.cpp \
    src/util/UtilErrorDomain.cpp \
    -lpcap

echo '[2/3] referencia em Python: o codigo que treinou o modelo'
# -I isola o interpretador: nada de sys.path vindo do diretorio corrente, para
# um modulo solto ao lado dos dados nao poder se passar por um da biblioteca.
python3 -I tests/gen_parity_dataset.py \
    --repro /repro --pcap-dir /pcap --out ${OUT#$ROOT/} $LIMITE

echo
echo '[3/3] comparacao, feature a feature'
/tmp/parity_dataset \
    ${OUT#$ROOT/}/dataset_bytemodels.bin \
    ${OUT#$ROOT/}/manifest.txt \
    /pcap \
    ${OUT#$ROOT/}
" 2>&1 | tee "$LOG"

VEREDITO=${PIPESTATUS[0]}
echo
echo "log: $LOG"
if [ "$VEREDITO" -eq 0 ]; then
    echo "PASSOU: paridade exata sobre o dataset."
else
    echo "FALHOU: veja as divergencias no log."
fi
exit "$VEREDITO"
