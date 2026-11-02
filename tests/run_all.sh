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
IMG="${IMG:-someip-base:3.5.6}"
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
PARSER="src/ids_sensor/SomeipParser.cpp src/ids_sensor/IdsSensorErrorDomain.cpp"
ARA="src/ara/idsm/EventReporter.cpp src/ara/core/InstanceSpecifier.cpp src/ara/core/CoreErrorDomain.cpp"
# A cadeia inteira, para o test_sensor. Precisa do libpcap pelo PacketSource.
CADEIA="src/ids_sensor/Sensor.cpp src/ids_sensor/Classifier.cpp src/ids_sensor/Normalizer.cpp \
        src/ids_sensor/SomeipParser.cpp src/ids_sensor/ByteModel.cpp src/ids_sensor/FeatureStreamer.cpp \
        src/ids_sensor/PacketSource.cpp src/ids_sensor/SecurityEventDispatcher.cpp \
        src/ids_sensor/IdsSensorErrorDomain.cpp src/util/UtilErrorDomain.cpp $ARA"
# Artefatos do modelo. Prefere o modelo REAL em build/cfg, convertido dos
# artefatos do treino por tools/build_sensor_artefacts.sh; cai para o modelo de
# PROVA de experiments/bench-inference/make_probe.sh quando o real nao existe.
# Com o real as linhas de prova vem do proprio dataset, entao as regioes de
# decisao visitadas sao as que o sensor visita.
if [ -f build/cfg/tables.bin ] && [ -f build/cfg/model_rows.f32 ]; then
    MODELO="build/cfg"
    MODELO_LINHAS="build/cfg/model_rows.f32"
    MODELO_PROBS="build/cfg/model_probs.txt"
    MODELO_QUAL="REAL (modelo adotado, linhas do dataset)"
else
    MODELO="${PROBE:-build/probe}"
    MODELO_LINHAS="$MODELO/probe_rows.f32"
    MODELO_PROBS="$MODELO/probe_probs.txt"
    MODELO_QUAL="de PROVA (dados aleatorios)"
fi

# ---------------------------------------------------------------- sem vsomeip
ARGS=()
roda test_sliding_window  tests/test_sliding_window.cpp src/util/UtilErrorDomain.cpp
# test_ara_core usa o ByteModel como tipo sem construtor padrao, entao precisa do
# ByteModel.cpp tambem, mesmo nao testando a pontuacao.
roda test_ara_core        tests/test_ara_core.cpp src/ids_sensor/ByteModel.cpp src/ids_sensor/IdsSensorErrorDomain.cpp
roda test_bytemodel       tests/test_bytemodel.cpp src/ids_sensor/ByteModel.cpp src/ids_sensor/IdsSensorErrorDomain.cpp
roda test_feature_streamer tests/test_feature_streamer.cpp $SENSOR
roda test_normalizer      tests/test_normalizer.cpp src/ids_sensor/Normalizer.cpp \
                          src/ids_sensor/IdsSensorErrorDomain.cpp
roda test_event_dispatch  tests/test_event_dispatch.cpp src/ids_sensor/SecurityEventDispatcher.cpp \
                          src/ids_sensor/IdsSensorErrorDomain.cpp $ARA

# ------------------------------------------------- precisa do libpcap
# O libpcap-dev esta nas DUAS imagens: na someip-base, porque o no de sensor vai
# capturar no barramento, e na someip-parity. No host cru ele pode nao estar, e
# ai o conjunto aparece como PULADO, nunca como aprovado.
printf '\n######## test_packet_source ########\n'
if [ -f /usr/include/pcap.h ] || [ -f /usr/include/pcap/pcap.h ]; then
    ARGS=()
    roda test_packet_source tests/test_packet_source.cpp src/ids_sensor/PacketSource.cpp \
         src/ids_sensor/IdsSensorErrorDomain.cpp -lpcap
else
    echo "  PULADO: libpcap-dev ausente no host. Rode dentro da imagem da bancada:"
    echo "    docker run --rm -v \"\$PWD\":/w -w /w $IMG bash tests/run_all.sh"
fi

# ------------------------------------- motores e cadeia (modelo de PROVA)
# As tabelas vem do modelo de prova, treinado em dados aleatorios: o que importa
# dele e a FORMA (16 features, 5 classes), que e o que determina a travessia, o
# layout e a ABI. O modelo real nao esta versionado.
printf '\n######## test_classifier ########\n'
if [ -f "$MODELO/tables.bin" ] && [ -f "$MODELO_LINHAS" ] && [ -f "$MODELO_PROBS" ]; then
    echo "  (modelo $MODELO_QUAL)"
    ARGS=("$MODELO/tables.bin" "$MODELO_LINHAS" "$MODELO_PROBS")
    roda test_classifier tests/test_classifier.cpp src/ids_sensor/Classifier.cpp \
         src/ids_sensor/IdsSensorErrorDomain.cpp
else
    echo "  PULADO: nenhum modelo. Converta o do treino com"
    echo "    bash tools/build_sensor_artefacts.sh <dir-dos-artefatos>"
    echo "  ou gere o de prova com"
    echo "    bash experiments/bench-inference/make_probe.sh"
fi

printf '\n######## test_sensor ########\n'
if [ ! -f "$MODELO/tables.bin" ]; then
    echo "  PULADO: nenhum modelo; veja a mensagem do test_classifier."
elif [ -f /usr/include/pcap.h ] || [ -f /usr/include/pcap/pcap.h ]; then
    echo "  (modelo $MODELO_QUAL)"
    ARGS=("$MODELO/tables.bin")
    roda test_sensor tests/test_sensor.cpp $CADEIA -lpcap
else
    echo "  PULADO: libpcap-dev ausente no host (o PacketSource entra na ligacao da cadeia)."
fi

# ---------------------------------------------------- cabecalho gerado
printf '\n######## test_comm_matrix ########\n'
# O conferidor e em Python e le a matriz em YAML. Sem o interprete ou sem o
# modulo, isto e PULADO e nao FALHA: falta ferramenta, nao esta errado. Antes
# desta checagem o `python3: command not found` ia para o stderr e o conjunto
# contava como falha sem dizer por que.
if ! command -v python3 >/dev/null 2>&1 || ! python3 -c "import yaml" >/dev/null 2>&1; then
    echo "  PULADO: falta python3 com PyYAML, que o tests/check_comm_matrix.py usa."
elif [ ! -f build/cfg/CommMatrix.hpp ]; then
    echo "  PULADO: rode antes  python3 tools/gen_config.py --out build/cfg"
elif g++ $FLAGS -o "$OBJ/tcm" tests/test_comm_matrix.cpp 2>"$OBJ/tcm.err"; then
    "$OBJ/tcm" > "$OBJ/tcm.out"
    if python3 tests/check_comm_matrix.py platform/comm_matrix.yaml "$OBJ/tcm.out"; then
        :
    else
        FALHAS=$((FALHAS + 1))
    fi
else
    echo "  NAO COMPILOU:"; head -10 "$OBJ/tcm.err" | sed 's/^/    /'; FALHAS=$((FALHAS + 1))
fi

# ------------------------------------------- a outra saida do gerador
# O test_comm_matrix confere os IDENTIFICADORES por round-trip contra o YAML.
# Este confere a ESTRUTURA da tabela de runtime, que e o que o no generico
# percorre: ponteiro nulo, contagem em desacordo com ponteiro, ou eventgroup
# apontando para evento inexistente compilam perfeitamente e so quebram quando
# um no tenta subir.
printf '\n######## test_matrix_table ########\n'
if [ -f build/cfg/CommMatrixTable.hpp ]; then
    ARGS=()
    roda test_matrix_table tests/test_matrix_table.cpp
else
    echo "  PULADO: rode antes  python3 tools/gen_config.py --out build/cfg"
fi

# ------------------------------------- banco de payloads e filtro BPF
# O banco alimenta os nos com bytes do barramento de referencia, e o filtro decide
# o que chega a cadeia. Os dois mudam o QUE E MEDIDO, entao uma falha silenciosa
# aqui falsifica o numero final em vez de quebrar a rodada.
printf '
######## test_payload_bank ########
'
ARGS=()
roda test_payload_bank tests/test_payload_bank.cpp src/ids_sensor/PacketSource.cpp src/ids_sensor/IdsSensorErrorDomain.cpp -lpcap

# -------------------------------------------- o plano do no generico
# A tabela acima e conferida na ESTRUTURA. Este confere o que o no generico
# DERIVA dela: quais eventos publicar, e em quais eventgroups assinar. Roda sem
# barramento, que e o motivo de a derivacao viver num cabecalho separado do no.
printf '
######## test_matrix_plan ########
'
if [ -f build/cfg/CommMatrixTable.hpp ]; then
    ARGS=()
    roda test_matrix_plan tests/test_matrix_plan.cpp
else
    echo "  PULADO: rode antes  python3 tools/gen_config.py --out build/cfg"
fi

# ------------------------------------------------- paridade do parser
printf '\n######## test_parity_parser ########\n'
if [ -f build/parity/parser_vectors.txt ]; then
    ARGS=(build/parity/parser_vectors.txt)
    roda test_parity_parser tests/test_parity_parser.cpp $PARSER
else
    echo "  PULADO: rode antes"
    echo "    python tests/gen_parser_vectors.py --repro ../someip-ids-multiclass-contentext --out build/parity"
fi

# ----------------------------------------------- paridade do extrator
printf '\n######## test_parity_streamer ########\n'
if [ -f build/parity/parity_vectors.txt ]; then
    ARGS=(build/parity/parity_bytemodels.bin build/parity/parity_vectors.txt)
    roda test_parity_streamer tests/test_parity_streamer.cpp $SENSOR
else
    echo "  PULADO: rode antes"
    echo "    python tests/gen_parity_vectors.py --repro ../someip-ids-multiclass-contentext --out build/parity"
fi

# ------------------------------------------------------- precisa do vsomeip
# Tres situacoes, nesta ordem. A primeira existe para que UMA invocacao dentro da
# imagem da bancada cubra a suite inteira: ali ha vsomeip e libpcap, mas nao ha
# docker, e a versao antiga deste bloco so sabia chamar `docker run` e portanto
# marcava PULADO justamente dentro da imagem onde o vsomeip mora.
printf '\n######## test_libecu ########\n'
if [ -d /usr/local/include/vsomeip ]; then
    echo "  (vsomeip local: compilando aqui, sem docker)"
    sh -c '
        g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -o /tmp/tl tests/test_libecu.cpp \
            src/libecu/Skeleton.cpp src/libecu/Proxy.cpp src/libecu/NodeRuntime.cpp src/libecu/EcuErrorDomain.cpp \
            src/ids_sensor/IdsSensorErrorDomain.cpp -lvsomeip3 -pthread || exit 1
        VSOMEIP_CONFIGURATION=build/cfg/vsomeip-gnss_ecu.json \
        VSOMEIP_APPLICATION_NAME=service-sample /tmp/tl 2>/dev/null' || FALHAS=$((FALHAS + 1))
elif docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "  (vsomeip na imagem $IMG)"
    docker run --rm -v "$ROOT":/w -w /w "$IMG" sh -c '
        g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -o /tmp/tl tests/test_libecu.cpp \
            src/libecu/Skeleton.cpp src/libecu/Proxy.cpp src/libecu/NodeRuntime.cpp src/libecu/EcuErrorDomain.cpp \
            src/ids_sensor/IdsSensorErrorDomain.cpp -lvsomeip3 -pthread || exit 1
        VSOMEIP_CONFIGURATION=/w/build/cfg/vsomeip-gnss_ecu.json \
        VSOMEIP_APPLICATION_NAME=service-sample /tmp/tl 2>/dev/null' || FALHAS=$((FALHAS + 1))
else
    echo "  PULADO: sem vsomeip local e sem a imagem $IMG. Os onze codigos de erro"
    echo "          da libecu NAO foram verificados; nao conte isto como aprovado."
fi

printf '\n================================\n'
if [ "$FALHAS" -eq 0 ]; then
    echo "TODOS OS CONJUNTOS PASSARAM"
else
    echo "$FALHAS CONJUNTO(S) COM FALHA"
fi
exit "$FALHAS"
