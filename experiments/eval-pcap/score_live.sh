#!/usr/bin/env bash
# Fase C de docs/PLANO.md: detecção medida sobre as rodadas VIVAS, com controle.
#
# O DETECCAO.md anterior foi invalidado: media detecção sobre ataques colados no
# PCAP, que não reproduziam o mecanismo do dataset. Aqui os ataques são os das
# rodadas de platform/scenario-live, feitos por ECUs injetoras no barramento, e o
# rótulo é o MAC de quem pôs cada quadro no fio (tools/label_by_mac.py).
#
# O CONTROLE é o mesmo detector, o mesmo score_pcap e a MESMA regra de rótulo sobre
# os PCAPs reais do dataset do Kim, cujos atacantes têm MAC conhecido. Sem ele, um
# número baixo na bancada não distingue "o detector não transfere" de "o scorer está
# errado".
#
# Pré-requisitos: as quatro rodadas (SCENARIO=... bash platform/scenario-live/run.sh)
# e os artefatos do treino em build/cfg (bytemodels.bin, minmax.bin, tables.bin).
#
# Uso, a partir da raiz do repo:
#     bash experiments/eval-pcap/score_live.sh
#     CONTROLE=0 bash experiments/eval-pcap/score_live.sh   # só a bancada
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT" || exit 1

IMG="${IMG:-someip-base:3.5.6}"
DATASET="${DATASET:-$HOME/dataset/data/pcap}"
CONTROLE="${CONTROLE:-1}"
LIVE="platform/scenario-live"
OUT="${OUT:-$HERE/out-live}"
mkdir -p "$OUT"

docker run --rm -v "$ROOT":/w -w /w "$IMG" sh -c '
    set -e
    mkdir -p build/bin
    g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -O2 -o build/bin/score_pcap \
        experiments/eval-pcap/score_pcap.cpp \
        src/ids_sensor/Sensor.cpp src/ids_sensor/Classifier.cpp src/ids_sensor/Normalizer.cpp \
        src/ids_sensor/SomeipParser.cpp src/ids_sensor/ByteModel.cpp src/ids_sensor/FeatureStreamer.cpp \
        src/ids_sensor/PacketSource.cpp src/ids_sensor/SecurityEventDispatcher.cpp \
        src/ids_sensor/IdsSensorErrorDomain.cpp src/util/UtilErrorDomain.cpp \
        src/ara/idsm/EventReporter.cpp src/ara/core/InstanceSpecifier.cpp src/ara/core/CoreErrorDomain.cpp \
        -lpcap
' || { echo "ERRO: compilação do score_pcap"; exit 1; }

pontua() {  # rótulo  pcap  labels
    echo "==================== $1"
    docker run --rm -v "$ROOT":/w -v "$DATASET":/dataset:ro -w /w "$IMG" \
        ./build/bin/score_pcap "$2" "$3" build/cfg --engine compact 2>&1 | tail -9
}

echo "############ BANCADA (rodadas vivas, rótulo pelo MAC dos injetores)"
for S in dos fuzzy mitm_single mitm_multi; do
    D="$LIVE/out-$S"
    if [ ! -f "$D/scn.pcap" ] || [ ! -f "$D/scn.labels" ]; then
        echo "  $S: falta a rodada ($D)"; continue
    fi
    pontua "bancada $S" "$D/scn.pcap" "$D/scn.labels" | tee "$OUT/bancada-$S.txt"
done

if [ "$CONTROLE" = "1" ]; then
    echo
    echo "############ CONTROLE (PCAPs reais do dataset, mesma regra de rótulo)"
    # MAC de cada atacante do dataset, medido com tools/sd_detail.py.
    printf '02:42:ac:12:00:0b dos\n'                              > "$OUT/macs-dos"
    printf '02:42:ac:12:00:11 fuzzy\n'                            > "$OUT/macs-fuzzy"
    printf '02:42:ac:12:00:0d mitm_single\n'                      > "$OUT/macs-mitm_single"
    printf '02:42:ac:12:00:0e mitm_relay\n02:42:ac:12:00:0f mitm_impostor\n' > "$OUT/macs-mitm_multi"
    for par in "dos dos_noti_flood" "fuzzy fuzzy_sd_offer_rand_noti(1)" \
               "mitm_single mitm_single_attacker" "mitm_multi mitm_multi_attacker"; do
        set -- $par
        P="$DATASET/$2.pcap"
        python3 tools/label_by_mac.py --pcap "$P" --macs "$OUT/macs-$1" --prefix 172.18.0. \
            --out "$OUT/dataset-$1.labels" | sed 's/^/  /' | tee "$OUT/dataset-$1-rotulo.txt"
        pontua "dataset $1" "/dataset/$2.pcap" "experiments/eval-pcap/out-live/dataset-$1.labels" \
            | tee "$OUT/dataset-$1.txt"
    done
fi
