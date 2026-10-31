#!/usr/bin/env bash
# Roda todos os testes do host, de uma vez, com os sanitizers ligados.
#
# Os que precisam do vsomeip rodam dentro da imagem someip-base; os outros rodam
# direto. Quem nao tiver a imagem ainda ve o resultado dos primeiros, e os de
# libecu aparecem como PULADOS, nunca como aprovados.
#
# Uso, a partir da raiz do repo:
#     bash tests/run_all.sh
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1
IMG="${IMG:-someip-base:3.6.4}"
SAN="-fsanitize=address,undefined"
FLAGS="-std=c++14 -I. -Wall -Wextra -Wpedantic -g"
OBJ="$(mktemp -d)"
trap 'rm -rf "$OBJ"' EXIT

FALHAS=0
roda() {
    local nome="$1"; shift
    printf '\n######## %s ########\n' "$nome"
    if ! g++ $FLAGS $SAN -o "$OBJ/$nome" "$@" 2>"$OBJ/$nome.err"; then
        echo "  NAO COMPILOU:"
        head -15 "$OBJ/$nome.err" | sed 's/^/    /'
        FALHAS=$((FALHAS + 1))
        return
    fi
    if "$OBJ/$nome" "${ARGS[@]:-}"; then
        :
    else
        FALHAS=$((FALHAS + 1))
    fi
}

SENSOR="src/ids_sensor/ByteModel.cpp src/ids_sensor/FeatureStreamer.cpp src/ids_sensor/IdsSensorErrorDomain.cpp src/util/UtilErrorDomain.cpp"

# ---------------------------------------------------------------- sem vsomeip
ARGS=()
roda test_sliding_window  tests/test_sliding_window.cpp src/util/UtilErrorDomain.cpp
# test_ara_core usa o ByteModel como tipo sem construtor padrao, entao precisa do
# ByteModel.cpp tambem, mesmo nao testando a pontuacao.
roda test_ara_core        tests/test_ara_core.cpp src/ids_sensor/ByteModel.cpp src/ids_sensor/IdsSensorErrorDomain.cpp
roda test_bytemodel       tests/test_bytemodel.cpp src/ids_sensor/ByteModel.cpp src/ids_sensor/IdsSensorErrorDomain.cpp
roda test_feature_streamer tests/test_feature_streamer.cpp $SENSOR

# ---------------------------------------------------- cabecalho gerado
printf '\n######## test_comm_matrix ########\n'
if [ -f build/cfg/CommMatrix.hpp ]; then
    if g++ $FLAGS -o "$OBJ/tcm" tests/test_comm_matrix.cpp 2>"$OBJ/tcm.err"; then
        "$OBJ/tcm" > "$OBJ/tcm.out"
        if python3 tests/check_comm_matrix.py platform/comm_matrix.yaml "$OBJ/tcm.out"; then
            :
        else
            FALHAS=$((FALHAS + 1))
        fi
    else
        echo "  NAO COMPILOU:"; head -10 "$OBJ/tcm.err" | sed 's/^/    /'; FALHAS=$((FALHAS + 1))
    fi
else
    echo "  PULADO: rode antes  python tools/gen_config.py --out build/cfg"
fi

# ------------------------------------------------------------- paridade
printf '\n######## test_parity_streamer ########\n'
if [ -f build/parity/parity_vectors.txt ]; then
    ARGS=(build/parity/parity_bytemodels.bin build/parity/parity_vectors.txt)
    roda test_parity_streamer tests/test_parity_streamer.cpp $SENSOR
else
    echo "  PULADO: rode antes"
    echo "    python tests/gen_parity_vectors.py --repro ../someip-ids-multiclass-contentext --out build/parity"
fi

# ------------------------------------------------------- precisa do vsomeip
printf '\n######## test_libecu (dentro da imagem) ########\n'
if docker image inspect "$IMG" >/dev/null 2>&1; then
    docker run --rm -v "$ROOT":/w -w /w "$IMG" sh -c '
        g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -o /tmp/tl tests/test_libecu.cpp \
            src/libecu/Skeleton.cpp src/libecu/Proxy.cpp src/libecu/EcuErrorDomain.cpp \
            src/ids_sensor/IdsSensorErrorDomain.cpp -lvsomeip3 -pthread || exit 1
        VSOMEIP_CONFIGURATION=/w/build/cfg/vsomeip-gnss_ecu.json \
        VSOMEIP_APPLICATION_NAME=service-sample /tmp/tl 2>/dev/null' || FALHAS=$((FALHAS + 1))
else
    echo "  PULADO: imagem $IMG ausente. Os onze codigos de erro da libecu NAO"
    echo "          foram verificados; nao conte isto como aprovado."
fi

printf '\n================================\n'
if [ "$FALHAS" -eq 0 ]; then
    echo "TODOS OS CONJUNTOS PASSARAM"
else
    echo "$FALHAS CONJUNTO(S) COM FALHA"
fi
exit "$FALHAS"
