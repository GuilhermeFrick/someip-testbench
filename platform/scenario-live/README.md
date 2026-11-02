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

## O que falta na fase A

O MITM multi: dois atacantes (.14 assina a VDE e repassa por 0x100B; .15 se passa pela VDE) e o
`StopOffer` com o IP da VDE forjado, por socket bruto (`INJ_SO_MODE=raw`), que o emissor já suporta
e o teste já confere contra o quadro do dataset.
