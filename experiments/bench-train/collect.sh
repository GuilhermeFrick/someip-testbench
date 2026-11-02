#!/usr/bin/env bash
# Coleta das rodadas vivas usadas para TREINAR e TESTAR o detector na própria bancada.
#
# Pergunta do experimento (experiments/bench-train/README.md): o falso positivo alto
# da bancada é limitação do MODELO da C1 ou do DADO com que ele foi treinado? Para
# responder, treina-se a mesma receita com dados da bancada. Cada cenário (benigno e
# os quatro ataques) roda DUAS vezes, em rodadas independentes: uma para treino,
# outra para teste. Nenhum quadro de teste vem de uma rodada vista no treino.
#
# Rodar desacoplado do SSH, porque leva uns 20 minutos:
#     nohup bash experiments/bench-train/collect.sh > /tmp/collect.out 2>&1 &
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT" || exit 1

RUNS="$HERE/runs"
DUR_TREINO="${DUR_TREINO:-120}"
DUR_TESTE="${DUR_TESTE:-60}"
PAYLOAD="${PAYLOAD:-counter}"
mkdir -p "$RUNS"

for SPLIT in treino teste; do
    DUR=$DUR_TREINO
    [ "$SPLIT" = "teste" ] && DUR=$DUR_TESTE
    for S in none dos fuzzy mitm_single mitm_multi; do
        D="$RUNS/$SPLIT-$S"
        echo "[$(date +%T)] $SPLIT $S (${DUR}s)"
        SENSOR=0 SCENARIO=$S PAYLOAD=$PAYLOAD DUR=$DUR OUT="$D" bash platform/scenario-live/run.sh > "$D.log" 2>&1
        if [ -s "$D/scn.pcap" ]; then
            echo "    $(grep -c . "$D/scn.labels") quadros; $(awk '$2 != 0' "$D/scn.labels" | wc -l) de ataque"
        else
            echo "    FALHOU: veja $D.log"
        fi
    done
done
echo FIM_COLETA
