# Os nove nós da matriz recuperada, no ar

Este degrau sobe o barramento do trabalho de referência com os nós da bancada e captura o
resultado. Não é "gerar tráfego parecido": a matriz vem de `tools/extract_matrix.py` rodado
sobre a captura benigna do próprio dataset, e os nós são **um binário só**
(`src/nodes/generic_ecu.cpp`) dirigido pela tabela gerada dela.

```sh
bash platform/bus-kim/run.sh                       # usa a sub-rede da matriz
SUBNET=172.28.0.0/24 DUR=120 bash platform/bus-kim/run.sh
PAYLOAD=random bash platform/bus-kim/run.sh        # degrau 1 da escada do DISCUSSAO.md
```

## O que a rodada produz

* `out/bus-kim.pcap` — a captura, na interface da **bridge**;
* `out/log-<no>.txt` — o log de cada nó, salvo antes da limpeza;
* um veredito com rota multicast, publicado/recebido por nó, e a divisão TCP/UDP.

## Três coisas que já custaram rodada, e por que estão como estão

**A rota multicast.** O Docker não cria rota para `224.0.0.0/4`, e o vsomeip só liga o Service
Discovery depois de ver uma. Sem ela a oferta entra em laço silencioso de `pending_sd_offers`,
sem erro nem aviso. Cada nó adiciona a rota antes de subir a pilha, e o veredito confere
`mc=1` em todos: com `mc=0` nada abaixo vale.

**A captura é na bridge, não dentro de um nó.** A bridge do Docker é um switch: um container
vê o próprio tráfego mais o multicast, e **não** vê o unicast entre dois outros. Como ~99% do
dataset é TCP unicast, capturar dentro de um nó mostraria uma fração do barramento. A captura
roda num container com `--network host`, na interface `br-<id>` da rede.

**O `exec`.** `docker kill --signal=SIGTERM` entrega o sinal ao PID 1. Com
`sh -c "...; ./no"` o PID 1 é o shell, que não repassa, e o nó morre sem imprimir o resumo.
Os logs também vão para arquivo **antes** da limpeza: na primeira versão deste script o
veredito lia `docker logs` de containers que o `trap` já havia removido, e relatou zero sobre
um barramento que tinha trocado 125 mil quadros.

## A sub-rede, e por que ela é um desvio declarado

A matriz descreve o barramento em `172.18.0.0/24` — que é justamente a faixa que o Docker
entrega ao **segundo** projeto de qualquer máquina. Onde ela já está tomada, `SUBNET=` manda o
`tools/remap_subnet.py` escrever uma matriz derivada, com cabeçalho dizendo o que mudou,
preservando o **último octeto** de cada nó. Endereço é chave de fluxo, não feature: os nós
continuam sendo `.2`…`.10`, em outro prefixo, e a contagem de fluxos não muda. O multicast do
Service Discovery **não** é remapeado.

## O transporte não pode ficar implícito

Cada serviço da matriz recuperada anuncia **dois** endpoints, um TCP e um UDP, e o
`offer_event` do vsomeip tem um parâmetro de *reliability* cujo padrão é `RT_UNKNOWN` — "decida
por mim". A primeira rodada dos nove nós, com ele implícito, deu **65% de TCP** contra os
**99,1%** do dataset, e entregava **cada notificação duas vezes**, uma por endpoint. Com o
`protocol:` da matriz levado até o `offer_event`, a mesma rodada dá **98% de TCP** e as
notificações caem exatamente pela metade. Por isso `ids::ecu::Transport` é parâmetro **sem
valor padrão**, e o `tests/test_matrix_table.cpp` reprova matriz que deixe o transporte
indefinido.

## O que um resultado aqui significa, e o que não significa

Notificações trocadas dizem que o barramento **subiu**. Não dizem que ele reproduz o de
referência. O critério é rodar o pipeline congelado sobre o PCAP e comparar o falso positivo
com o do dataset — é o passo 2 da §7 do `DISCUSSAO.md`.
