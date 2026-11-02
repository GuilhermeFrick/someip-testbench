#!/usr/bin/env bash
# Sobe os NOVE nós da matriz recuperada e captura o barramento inteiro.
#
# É o degrau que decide se a bancada reproduz um barramento validado pela
# academia ou só gera tráfego parecido. O critério não é "os nós subiram": é o
# falso positivo do sensor sobre ESTE tráfego, comparado com o do dataset. Se
# cair ao nível do dataset, a escada da §4.1 do DISCUSSAO.md fica em pé; se não
# cair, o conteúdo do payload (e não a identidade dos nós) é o que não transfere.
#
# TRÊS COISAS QUE JÁ CUSTARAM RODADA NESTA BANCADA, e que este script já traz
# resolvidas:
#
#   1. ROTA MULTICAST. O Docker não cria rota para 224.0.0.0/4, e o vsomeip só
#      liga o Service Discovery depois de ver uma. Sem ela a oferta entra em laço
#      silencioso de `pending_sd_offers`, sem erro nem aviso. Cada nó adiciona a
#      rota ANTES de subir a pilha.
#
#   2. CAPTURA. Dentro de um container só se vê o próprio tráfego mais o
#      multicast; a bridge do Docker é um switch e não entrega a um terceiro o
#      unicast entre dois outros. Como 99% do dataset é TCP unicast, capturar
#      dentro de um nó mostraria uma fração do barramento. A captura roda num
#      container com --network host, na INTERFACE DA BRIDGE, que é onde todo o
#      tráfego dos nove passa.
#
#   3. `exec`. `docker kill --signal=SIGTERM` entrega o sinal ao PID 1. Com
#      `sh -c "...; ./no"` o PID 1 é o shell, que não repassa, e o nó morre sem
#      imprimir o resumo. Daí o `exec` na última linha de cada nó.
#
# Pré-requisitos:
#     python tools/gen_config.py --matrix <matriz> --out build/cfg
#     (este script faz isso sozinho, inclusive o remapeamento de sub-rede)
#
# Uso, a partir da raiz do repo:
#     bash platform/bus-kim/run.sh
#     DUR=120 PAYLOAD=random bash platform/bus-kim/run.sh
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT" || exit 1

IMG="${IMG:-someip-base:3.6.4}"
NET="${NET:-someip-kim}"
MATRIZ="${MATRIZ:-platform/comm_matrix_kim.yaml}"
DUR="${DUR:-60}"
PAYLOAD="${PAYLOAD:-counter}"
OUT="${OUT:-$HERE/out}"
CFG="build/run/cfg"
MATRIZ_RUN="build/run/comm_matrix_run.yaml"

# A sub-rede da matriz é a primeira escolha. `172.18.0.0/24` é exatamente a faixa
# que o Docker entrega ao segundo projeto de qualquer máquina, então quando ela já
# está tomada o SUBNET aqui remapeia -- e o remapeamento fica REGISTRADO na matriz
# derivada, com cabeçalho dizendo o que mudou. Ver tools/remap_subnet.py.
SUBNET="${SUBNET:-}"

# ------------------------------------------------------------------ 0. limpeza
NOMES=""
limpa() {
    if [ -n "$NOMES" ]; then
        docker rm -f $NOMES >/dev/null 2>&1
    fi
    docker rm -f kim-cap >/dev/null 2>&1
    docker network rm "$NET" >/dev/null 2>&1
}
limpa
trap limpa EXIT

mkdir -p "$OUT" build/run

# --------------------------------------------------- 1. matriz e configuração
if [ -n "$SUBNET" ]; then
    echo "[matriz] remapeando a sub-rede para $SUBNET (desvio declarado)"
    python3 tools/remap_subnet.py --matrix "$MATRIZ" --subnet "$SUBNET" --out "$MATRIZ_RUN" || exit 1
    USAR="$MATRIZ_RUN"
else
    USAR="$MATRIZ"
fi

SUB_EFETIVA="$(grep -m1 'subnet:' "$USAR" | sed 's/.*subnet:[[:space:]]*//' | tr -d '\r')"
echo "[matriz] $USAR  sub-rede $SUB_EFETIVA"

python3 tools/gen_config.py --matrix "$USAR" --out "$CFG" >/dev/null || {
    echo "ERRO: o gerador falhou sobre $USAR"
    exit 1
}
# O nó genérico percorre a tabela de runtime, então ela é COMPILADA junto. A
# tabela gerada acima tem de ser a que entra no binário.
cp "$CFG/CommMatrixTable.hpp" build/cfg/CommMatrixTable.hpp || exit 1

NOS="$(python3 - "$USAR" <<'PY'
import sys, yaml
with open(sys.argv[1], encoding="utf-8") as fh:
    m = yaml.safe_load(fh)
# Provedores primeiro: um consumidor que suba antes espera, e esperar é correto,
# mas subir na ordem encurta a rodada e deixa os logs legíveis.
nos = list(m["nodes"].items())
provedores = [n for n, d in nos if d.get("provides")]
consumidores = [n for n, d in nos if not d.get("provides")]
print(" ".join(provedores + consumidores))
PY
)" || exit 1
echo "[matriz] nós, provedores primeiro: $NOS"

# -------------------------------------------------------------- 2. compilação
echo "[build] compilando a libecu e o nó genérico dentro de $IMG"
docker run --rm -v "$ROOT":/w -w /w "$IMG" sh -c '
    set -e
    rm -rf build/obj build/bin
    mkdir -p build/obj build/bin
    for f in EcuErrorDomain Skeleton Proxy NodeRuntime; do
        g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -O2 -c -o build/obj/$f.o src/libecu/$f.cpp
    done
    g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -O2 -o build/bin/generic_ecu \
        src/nodes/generic_ecu.cpp build/obj/*.o -lvsomeip3 -pthread
' || { echo "ERRO: a compilacao falhou"; exit 1; }
echo "[build] ok"

# ------------------------------------------------------------------- 3. rede
docker network create --subnet "$SUB_EFETIVA" "$NET" >/dev/null 2>&1 || {
    echo "ERRO: nao foi possivel criar a rede $NET em $SUB_EFETIVA."
    echo "      Quase sempre e sobreposicao com uma rede que ja existe:"
    docker network ls --format '{{.Name}}' | while read -r n; do
        s="$(docker network inspect "$n" --format '{{range .IPAM.Config}}{{.Subnet}} {{end}}' 2>/dev/null)"
        [ -n "$s" ] && echo "        $n: $s"
    done
    echo "      Rode de novo com outra faixa, p.ex.:"
    echo "        SUBNET=172.28.0.0/24 bash platform/bus-kim/run.sh"
    exit 1
}
BRIDGE="br-$(docker network inspect "$NET" --format '{{.Id}}' | cut -c1-12)"
echo "[rede] $NET ($SUB_EFETIVA) na interface $BRIDGE"

# ---------------------------------------------------------------- 4. captura
# --network host para ver a interface da bridge, que e onde o trafego dos nove
# passa. `-U` para escrever sem bufferizar, senao um encerramento abrupto perde o
# fim do arquivo.
docker run -d --name kim-cap --network host --cap-add NET_ADMIN --cap-add NET_RAW \
    -v "$OUT":/out "$IMG" \
    sh -c "exec tcpdump -i $BRIDGE -w /out/bus-kim.pcap -U -s 0" >/dev/null || {
    echo "ERRO: a captura nao subiu"
    exit 1
}
sleep 2
if [ "$(docker inspect -f '{{.State.Running}}' kim-cap 2>/dev/null)" != "true" ]; then
    echo "ERRO: a captura morreu. Log:"
    docker logs kim-cap 2>&1 | tail -5 | sed 's/^/    /'
    exit 1
fi
echo "[captura] tcpdump em $BRIDGE -> $OUT/bus-kim.pcap"

# -------------------------------------------------------------------- 5. nós
MC_ROUTE="ip route add 224.0.0.0/4 dev eth0 2>/dev/null"
for NO in $NOS; do
    IP="$(grep -A3 "^  $NO:" "$USAR" | grep -m1 'ip:' | sed 's/.*ip:[[:space:]]*//' | tr -d '\r')"
    NOMES="$NOMES ecu-$NO"
    docker run -d --name "ecu-$NO" --network "$NET" --ip "$IP" \
        --cap-add NET_ADMIN \
        -v "$ROOT":/w -w /w "$IMG" \
        sh -c "$MC_ROUTE
               export VSOMEIP_CONFIGURATION=/w/$CFG/vsomeip-$NO.json
               export ECU_NODE=$NO
               export ECU_PAYLOAD=$PAYLOAD
               export ECU_REPORT_SECONDS=15
               exec ./build/bin/generic_ecu" >/dev/null || {
        echo "ERRO: $NO nao subiu"
        exit 1
    }
    printf '[no] %-10s %s\n' "$NO" "$IP"
    sleep 1
done

echo "[coletando ${DUR}s]"
sleep "$DUR"

# ---------------------------------------------------------------- 6. encerrar
# SIGTERM para que cada nó imprima o resumo; é o `exec` acima que faz o sinal
# chegar nele e não no shell.
for NO in $NOS; do
    docker kill --signal=SIGTERM "ecu-$NO" >/dev/null 2>&1
done
sleep 3
docker kill --signal=SIGINT kim-cap >/dev/null 2>&1
sleep 2

# Os logs vao para arquivo ANTES da limpeza. Na primeira rodada deste script o
# veredito lia `docker logs` de containers que o trap ja havia removido, e
# relatou zero sobre um barramento que tinha trocado 125 mil quadros. Log de
# container e volatil; arquivo nao.
for NO in $NOS; do
    docker logs "ecu-$NO" > "$OUT/log-$NO.txt" 2>&1
done
echo "[logs] $OUT/log-<no>.txt"

# ---------------------------------------------------------------- 7. veredito
echo
echo "===== rota multicast: mc=1 em todos? ====="
echo "  (com mc=0 o Service Discovery nunca sobe e nada abaixo funciona)"
FALTA_MC=0
for NO in $NOS; do
    L="$(grep -oE 'mc=[01]' "$OUT/log-$NO.txt" | tail -1)"
    [ "$L" = "mc=1" ] || { FALTA_MC=$((FALTA_MC + 1)); printf '  %-10s %s\n' "$NO" "${L:-sem linha de netlink}"; }
done
if [ "$FALTA_MC" -eq 0 ]; then
    echo "  todos os nós viram a rota"
else
    echo "  $FALTA_MC nó(s) SEM a rota: o resultado abaixo nao vale"
fi

echo
echo "===== por nó: publicado e recebido ====="
TOT_ENV=0
TOT_REC=0
for NO in $NOS; do
    ENV="$(docker logs "ecu-$NO" 2>&1 | grep -oE 'encerrando, [0-9]+ enviados' | grep -oE '[0-9]+' | tail -1)"
    REC="$(docker logs "ecu-$NO" 2>&1 | grep -E ': [0-9]+ recebidos$' | grep -oE '[0-9]+ recebidos' | grep -oE '[0-9]+' \
           | awk '{s+=$1} END {print s+0}')"
    ENV="${ENV:-0}"
    REC="${REC:-0}"
    TOT_ENV=$((TOT_ENV + ENV))
    TOT_REC=$((TOT_REC + REC))
    printf '  %-10s enviados %-8s recebidos %s\n' "$NO" "$ENV" "$REC"
done
echo "  ----"
printf '  %-10s enviados %-8s recebidos %s\n' "TOTAL" "$TOT_ENV" "$TOT_REC"

echo
echo "===== o que cada lado disse de si (primeiras linhas) ====="
for NO in $NOS; do
    N="$(grep -cE "^\[$NO\]" "$OUT/log-$NO.txt")"
    printf '  %-10s %s linha(s) propria(s)
' "$NO" "$N"
done
PRIMEIRO="$(echo $NOS | awk '{print $1}')"
ULTIMO="$(echo $NOS | awk '{print $NF}')"
echo "  --- $PRIMEIRO ---"
grep -E "^\[$PRIMEIRO\]" "$OUT/log-$PRIMEIRO.txt" | head -8 | sed 's/^/    /'
echo "  --- $ULTIMO ---"
grep -E "^\[$ULTIMO\]" "$OUT/log-$ULTIMO.txt" | head -8 | sed 's/^/    /'

echo
echo "===== o que foi para o barramento ====="
# Lido do PCAP, nao dos logs: log diz o que o no ACHA que enviou, o PCAP diz o
# que passou pela bridge. Quando os dois discordam, e o PCAP que vale.
docker run --rm -v "$OUT":/out "$IMG" sh -c '
    P=/out/bus-kim.pcap
    T=$(tcpdump -r $P -nn 2>/dev/null | wc -l)
    TCP=$(tcpdump -r $P -nn tcp 2>/dev/null | wc -l)
    UDP=$(tcpdump -r $P -nn udp 2>/dev/null | wc -l)
    SD=$(tcpdump -r $P -nn "udp port 30490" 2>/dev/null | wc -l)
    echo "  quadros:          $T"
    echo "  TCP:              $TCP"
    echo "  UDP:              $UDP   (dos quais SD na 30490: $SD)"
    if [ "$T" -gt 0 ]; then
        echo "  TCP sobre o total: $(( TCP * 100 / T ))%   (no dataset do baseline: 99,1%)"
    fi
    echo "  tamanho:          $(du -h $P | cut -f1)"
'

echo
if [ "$TOT_REC" -gt 0 ] && [ "$FALTA_MC" -eq 0 ]; then
    echo "PASSOU: os nove nós trocaram $TOT_REC notificações por SOME/IP."
    echo "        Isto diz que o barramento SUBIU. Nao diz ainda que ele reproduz o"
    echo "        de referência: o criterio e rodar o sensor sobre este PCAP e"
    echo "        comparar o falso positivo com o do dataset."
else
    echo "FALHOU: nenhuma notificação chegou, ou falta rota multicast. Veja acima."
fi
echo "  PCAP: $OUT/bus-kim.pcap"
echo "  cfg:  $CFG"
