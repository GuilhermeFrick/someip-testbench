# Rodada viva: as 9 ECUs e um injetor no mesmo barramento

Fases A1 (Fuzzy) e A2 (DoS) de `docs/PLANO.md`. O injetor (`src/nodes/injector_ecu.cpp`) é uma ECU
com IP próprio que só faz o que a pilha SOME/IP permite. A configuração dele sai de
`tools/gen_injector_config.py`. Cada cenário entra na ordem em que o atacante entra no PCAP do
dataset do Kim (`tools/attack_timeline.py`).

```sh
SCENARIO=dos   bash platform/scenario-live/run.sh
SCENARIO=fuzzy bash platform/scenario-live/run.sh
```

O MAC do injetor é fixado (derivado do IP) e gravado em `out-*/injector.mac`: é ele o rótulo,
porque o IP de origem pode ser forjado.

**Critério de aceite:** `tools/attack_flows.py` sobre a captura produz a mesma assinatura que sobre o
PCAP do dataset. Rodadas de 60 s, vsomeip 3.5.6.

## DoS (injetor .11, no ar antes da VDE)

| | dataset (`dos_noti_flood`) | bancada |
|---|---|---|
| UDP / TCP | 59,5% / 40,5% | **58,9% / 41,1%** |
| ofertas de 0x1001 pelo atacante | 49/s, a cada 20 ms | 47,5/s, a cada 20 ms |
| assinatura da ADAS no atacante | aceita (52 177 Ack) | aceita (3 062 Ack) |
| assinaturas de CLU, PT, TEL | recusadas, ~6,5/s cada | recusadas, ~8,9/s cada |
| reconexões TCP de CLU, PT, TEL ao atacante | ~6,6/s cada | ~9/s cada |
| SYN para a VDE | ~1 | 0 |
| notificações do atacante à ADAS | 46,8/s, payload constante | 50/s, o mesmo payload |
| CLU e PT recebendo 0x1001 | nada | nada |

Dois ajustes foram necessários para chegar aqui, e os dois são achados:

1. **O injetor declara só o evento que publica** (0x0001). Declarando os quatro eventos da VDE, ele
   aceita a assinatura de todos; o atacante do dataset recusa CLU, PT e TEL, e é essa recusa que
   produz as reconexões em massa. `INJ_DECLARE=all` mantém a outra forma, para comparação.
2. **vsomeip 3.5.6, não 3.6.4.** A 3.6.4 recusa até a assinatura da ADAS ("didn't find TCP
   connection: Subscription rejected"), e só 296 notificações chegam em 60 s (2,2% de TCP). A 3.5.6
   e a 3.1.20.3 aceitam, como o dataset. A 3.5.6 era a mais recente quando o dataset foi capturado
   (2025-08-08), e no barramento benigno ela dá 99,1% de TCP, igual ao dataset (a 3.6.4 dava 98%).
   Por isso virou a versão padrão da bancada.

| versão | ADAS: Ack / Nack | notificações à ADAS em 60 s | UDP / TCP |
|---|---|---|---|
| 3.6.4 | 260 / 2 967 | 281 | 97,8% / 2,2% |
| 3.5.6 | 3 062 / 0 | 3 001 | 58,9% / 41,1% |

## Fuzzy (injetor .17, depois que o barramento subiu)

| | dataset (`fuzzy_sd_offer_rand_noti(1)`) | bancada |
|---|---|---|
| serviços anunciados | 200, 0x2001 a 0x20C8, sem buracos | 200, 0x2001 a 0x20C8, sem buracos |
| instância | igual ao serviço | igual ao serviço |
| quadros do atacante | 3,51/s | 3,81/s |
| transporte | 100% UDP, só SD | 100% UDP, só SD |
| alguém assina ou conecta | não | não |
| efeito no resto do barramento | nenhum | nenhum (todas as ECUs recebem normalmente) |

## MITM single (injetor .13, ~3 s depois da VDE)

O que captura a vítima é um `StopOfferService` em nome da VDE, que a pilha não manda: ele é montado
em `src/nodes/SdRawEmitter.hpp` e conferido byte a byte contra o quadro do dataset
(`tests/test_sd_raw.cpp`). O checksum UDP do quadro do dataset é a soma parcial do pseudo-cabeçalho
(checksum offload visto na bridge): o atacante usou um socket UDP comum, do próprio IP, e o injetor
faz o mesmo. O conteúdo publicado à ADAS é o sinal falso do próprio atacante do dataset (velocidade
média de 14,7 m/s com o carro parado), extraído pelo MAC dele com `tools/extract_attacker_payloads.py`.

| | dataset (`mitm_single_attacker`) | bancada |
|---|---|---|
| ofertas falsas de 0x1001 | 98/s, a cada 10 ms | 94/s, a cada 10 ms |
| `StopOffer` em nome da VDE, à ADAS | a cada 2,5 s, do IP do atacante, flags 0xC0 | a cada 2,5 s, do IP do atacante, flags 0xC0 |
| ADAS | assina e conecta no atacante | assina (5 354 Ack) e conecta no atacante |
| CLU, PT, TEL | recusadas pelo atacante, reconectando na VDE (~6,4/s) | recusadas, reconectando na VDE (~8,7/s) |
| sinal falso à ADAS | 93/s | 100/s |
| CLU, PT, TEL recebendo da VDE | quase nada | quase nada (162 a 910 em 60 s) |
| UDP / TCP **durante o sequestro** | 70,2% / 29,8% | 69,0% / 31,0% |

No dataset o sequestro dura 565 dos 1 479 s da captura; fora dele o atacante só manda SD (80,8% UDP,
0,3% TCP), e é isso que leva a proporção da captura inteira a 80,3%/19,7%. A comparação certa é
dentro da janela.

## MITM multi (injetores .14 e .15)

Dois injetores, na ordem do dataset: o .14 (`mitm_relay`) sobe antes da VDE, assina a VDE como
cliente comum e repassa o último valor recebido pelo serviço 0x100B (instância 0x000B, TCP 30601,
UDP 31697) a cada ~9 ms; o .15 (`mitm_impostor`) sobe ~3,4 s depois da VDE, oferece 0x1001 no ritmo
normal, assina o repasse e, **para cada notificação repassada que recebe**, manda à ADAS um
`StopOffer` com o IP da VDE forjado, por socket bruto.

```sh
SCENARIO=mitm_multi bash platform/scenario-live/run.sh
```

| | dataset (`mitm_multi_attacker`) | bancada |
|---|---|---|
| TCP / UDP (regra por IP do dataset) | 98,2% / 1,8% | 97,8% / 2,2% |
| quadros do .14 / do .15 | 61,1% / 38,9% | 60,8% / 39,0% |
| .14 assina a VDE; .15 assina o 0x100B no .14 | sim | sim |
| .14 também tenta assinar 0x1001 no .15 (efeito da oferta falsa) | sim | sim |
| SYN da ADAS para o .15 / para a VDE | 426 / 427 | 32 / 30 |
| SYN de CLU, PT, TEL e .14 para a VDE | 431 cada | 33 cada |
| `StopOffer` forjados / notificações repassadas | 163 188 / 163 190 | 6 663 / 6 663 |
| ritmo do repasse .14 → .15 | 110/s | 117/s |
| VDE → .14 | 60/s | 64/s |
| intervalo entre `StopOffer` forjados | 7,7 ms | 8,9 ms |
| quem envia o `StopOffer` forjado | MAC do .15 com IP .10 | MAC do .15 com IP .10 |

Os `StopOffer` forjados saem com o IP da VDE, e a regra de rótulo por IP do dataset os marca como
tráfego normal. Na bancada o rótulo é o MAC: `out-*/injector.mac` lista o MAC de cada injetor.

## Fase A completa

As quatro famílias rodam ao vivo no barramento e passaram no critério de aceite. Depois da
reorganização para dois injetores, DoS, MITM single e Fuzzy foram rodados de novo (40 s): 59,2%/40,8%,
68,7%/31,3% e 100% UDP, e a suíte inteira passou na imagem 3.5.6.

## Controle dos injetores durante a rodada (fase D)

Cada injetor fica também na rede `ctrl` (`CTRL_SUBNET`, padrão 172.29.0.0/24), que **nunca é
capturada**, e aceita comandos de uma linha em UDP (`src/nodes/CtrlChannel.hpp`): `on`, `off`,
`status`, `set period_ms N`, `set so_period_ms N`, `set so_target A.B.C.D`. `off` faz o que uma ECU
faria para parar: retira as ofertas e para de publicar e de mandar `StopOffer`; `on` anuncia de novo.
Os comandos saem de `tools/inj_ctl.py`, e um roteiro pode ser dado na rodada:

```sh
CTRL_PLAN="15:off;30:on" SCENARIO=mitm_multi bash platform/scenario-live/run.sh
```

Cada comando entra na linha do tempo com a resposta do injetor, e `tools/attack_activity.py` mostra o
tráfego de cada injetor em janelas de 5 s com os comandos marcados. Rodada de teste (MITM multi, dois
injetores, repetida duas vezes com o mesmo resultado):

| janela (s) | .15 (impostor) | .14 (repasse) | comando |
|---|---:|---:|---|
| 10–15 | 1 140 | 749 | |
| 15–20 | 12 | 11 | `off` nos dois |
| 20–30 | 0 | 0 | |
| 30–35 | 1 107 | 536 | `on` nos dois |

**Quadros de controle na captura do barramento: 0.**

Dois achados no caminho:

- **Com duas redes o Docker não garante que o barramento seja a `eth0`**, e a rota padrão passa para a
  rede de controle. Numa rodada o .15 saiu com o SD inteiro na rede de controle e nunca achou o .14.
  A rota multicast do injetor agora vai na interface que tem o IP do barramento.
- **Quando desligado, o injetor do MITM single ainda põe IGMP no fio** (~90 quadros/s): a pilha vsomeip
  configurada para anunciar a cada 10 ms sai e entra no grupo do SD o tempo todo ("Didn't receive a
  multicast SD message for 22ms"). Não é ataque, é a pilha. O dataset tem o mesmo efeito: o IGMP é
  17,7% dos quadros do atacante do DoS e 15,9% do MITM single (`tools/attack_activity.py`,
  composição medida na fase C).

## Falso positivo benigno ao vivo (em andamento, 02/11)

`SCENARIO=none` sobe só o barramento benigno, com o sensor ao vivo; como não há ataque, todo security
event é falso positivo. `PAYLOAD` escolhe o conteúdo das ECUs e `PAYLOAD_FILE` o banco usado pela
política `dataset` (gerado com `tools/extract_payloads.py` na imagem `someip-parity:1`, que tem scapy).

| conteúdo benigno | pacotes | security events | falso positivo |
|---|---:|---:|---:|
| `counter` (sintético) | 102 035 | 9 623 | **9,43%** |
| `dataset` (bytes do Kim) | 102 012 | 402 | **0,39%** |

Reproduz ao vivo, no vsomeip 3.5.6, o diagnóstico feito offline na 3.6.4 (`experiments/eval-pcap/
RESULTADOS.md`: 9,42% → 0,36%). Com `counter`, **94% dos eventos falsos são da VDE classificada como
MITM single** (9 021 de 9 623). Com os bytes do dataset sobram ~400, entre eles ~30 por consumidora
como MITM multi (provável resíduo de Service Discovery).

**Hipótese em teste:** no dataset a VDE quase não muda de valor (99,6% das notificações repetem a
anterior; o carro está parado), e o único sinal de velocidade que muda o tempo todo é o do atacante
do MITM single. Se o detector aprendeu "0x1001 com valor mudando = MITM single", um veículo em
movimento (CARLA) vai parecer ataque. O teste: rodadas com a VDE `constant`, `random` e com um sinal de
velocidade plausível em movimento (`build/cfg/payloads-vde-andando.bin`: o banco do dataset com o
fluxo 0x1001/0x0001 trocado pelo sinal que o atacante do dataset publicou). Interrompido: o `runner`
saiu da rede durante a rodada.
