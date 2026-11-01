#!/usr/bin/env bash
# Converte os artefatos do TREINO no que o ids_sensor carrega, e gera o C99 do
# modelo.
#
# Os tres artefatos do sensor tem de vir da MESMA rodada de treino: o modelo de
# arvores, as tabelas do modelo de bytes e os limites de normalizacao. Misturar
# rodadas desloca a fronteira de decisao sem levantar erro nenhum -- e por isso
# que o IdsSensorErrc::TrainingRunMismatch existe (e segue sem produtor: nenhuma
# verificacao cruzada esta implementada).
#
# Entrada, um diretorio com o que o `pipeline.run` salva:
#     model_16.json      modelo XGBoost
#     bytemodels.npz     tabelas (L, 256) por camada
#     minmax.npy         limites do treino, duas linhas
#
# Saida, em build/cfg:
#     tables.bin         modelo como tabelas de nos (avaliador compacto)
#     bytemodels.bin     tabelas de bytes no formato do ByteModels::Create
#     minmax.bin         limites no formato do Normalizer::Create
#     tl2cgen_c/         C99 do modelo, para o Tl2cgenClassifier
#     model_rows.f32     linhas de prova tiradas do dataset
#     model_probs.txt    as probabilidades que o XGBoost deu para elas
#
# Uso, a partir da raiz do repo:
#     bash tools/build_sensor_artefacts.sh ~/work/artefatos
set -u

IMG="${IMG:-someip-parity:1}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
ART="${1:-}"
OUT="${OUT:-build/cfg}"
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"

if [ -z "$ART" ]; then
    echo "uso: bash tools/build_sensor_artefacts.sh <diretorio-dos-artefatos>"
    exit 2
fi
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "ERRO: imagem $IMG ausente. Construa antes:"
    echo "    docker build -f deploy/Dockerfile.parity -t someip-parity:1 ."
    exit 1
fi
for f in model_16.json bytemodels.npz minmax.npy; do
    if [ ! -f "$ART/$f" ]; then
        echo "ERRO: falta $ART/$f"
        exit 1
    fi
done

# -i obrigatorio: sem ele o docker nao liga o stdin e o `bash -s` sai com zero
# sem executar nada.
docker run --rm -i -v "$ROOT":/w -v "$(cd "$ART" && pwd)":/art:ro -w /w "$IMG" bash -s <<SCRIPT
set -e
trap 'chown -R $HOST_UID:$HOST_GID /w/build 2>/dev/null || true' EXIT

mkdir -p $OUT
python3 -c "
import xgboost, treelite, tl2cgen
print('   xgboost', xgboost.__version__, 'treelite', treelite.__version__, 'tl2cgen', tl2cgen.__version__)
"

echo '[1/5] tabelas de bytes'
python3 -I tools/export_bytemodels.py --npz /art/bytemodels.npz --out $OUT/bytemodels.bin

echo '[2/5] limites de normalizacao'
python3 -I tools/export_minmax.py --from-npy /art/minmax.npy --out $OUT/minmax.bin --names

echo '[3/5] modelo como tabelas de nos'
python3 -I experiments/bench-inference/export_tables.py --model /art/model_16.json --out $OUT

echo '[4/5] C99 do modelo, pelo TL2cgen'
python3 -I -c "
import os, treelite, tl2cgen
m = treelite.frontend.load_xgboost_model('/art/model_16.json')
tl2cgen.generate_c_code(m, dirpath='$OUT/tl2cgen_c', params={})
print('   arquivos:', sorted(os.listdir('$OUT/tl2cgen_c')))
"

echo '[5/5] linhas de prova e as probabilidades de referencia'
python3 -I tools/make_model_probe.py --model /art/model_16.json --out $OUT
SCRIPT

echo
echo "artefatos em $ROOT/$OUT"
