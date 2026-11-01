#!/usr/bin/env bash
# Monta o modelo de PROVA e tudo que vem dele, dentro da imagem da paridade.
#
# Serve a uma pergunta só: qual é, de fato, a ABI do C que o TL2cgen gera? O
# `ids::Tl2cgenClassifier` declara `union Entry` e a assinatura de `predict` por
# conta própria, para não depender de um `header.h` que só existe depois deste
# passo — e errar o layout dessa union não dá erro de compilação, dá corrupção
# silenciosa das features. Então se confere contra o header real.
#
# Saída, em build/probe:
#   probe_model.json     modelo descartável, treinado em dados aleatórios
#   probe_rows.f32       linhas de prova
#   probe_probs.txt      as probabilidades que o Python deu para elas
#   tables.bin           o mesmo modelo como tabelas de nós
#   tl2cgen_c/           o C99 gerado, inclusive o header.h que se quer ver
#
# Uso, a partir da raiz do repo:
#     bash experiments/bench-inference/make_probe.sh
set -u

IMG="${IMG:-someip-parity:1}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${OUT:-build/probe}"
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "ERRO: imagem $IMG ausente. Construa antes:"
    echo "    docker build -f deploy/Dockerfile.parity -t someip-parity:1 ."
    exit 1
fi

# As versões ficam fixas pelo mesmo motivo de sempre: a prova precisa poder ser
# repetida, e uma ABI que muda de versão é justamente o que se está conferindo.
# O -i e OBRIGATORIO aqui: sem ele o docker nao liga o stdin do container, o
# `bash -s` le EOF na hora e sai com codigo ZERO sem executar nada. Falha
# silenciosa, e foi exatamente o que aconteceu na primeira tentativa: log vazio,
# exit 0, nenhum arquivo gerado.
docker run --rm -i -v "$ROOT":/w -w /w "$IMG" bash -s <<SCRIPT
set -e
trap 'chown -R $HOST_UID:$HOST_GID /w/build 2>/dev/null || true' EXIT

echo '[1/4] versoes do toolchain do modelo'
python3 -c "
import xgboost, treelite, tl2cgen
print('   xgboost', xgboost.__version__, 'treelite', treelite.__version__, 'tl2cgen', tl2cgen.__version__)
"

echo '[2/4] treinando o modelo de prova'
python3 -I experiments/bench-inference/make_probe_model.py --out $OUT

echo '[3/4] exportando as tabelas de nos'
python3 -I experiments/bench-inference/export_tables.py --model $OUT/probe_model.json --out $OUT

echo '[4/4] gerando o C99 com o TL2cgen'
python3 -I -c "
import os, treelite, tl2cgen
m = treelite.frontend.load_xgboost_model('$OUT/probe_model.json')
tl2cgen.generate_c_code(m, dirpath='$OUT/tl2cgen_c', params={})
print('   arquivos:', sorted(os.listdir('$OUT/tl2cgen_c')))
print('   treelite', treelite.__version__, 'tl2cgen', tl2cgen.__version__)
"

echo
echo '===== header.h gerado: a ABI que o Classifier tem de declarar ====='
cat $OUT/tl2cgen_c/header.h
SCRIPT
