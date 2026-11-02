# Degrau A0: duas ECUs oferecendo o mesmo serviço

**Pergunta:** o vsomeip 3.6.4 reproduz o mecanismo dos ataques do dataset do Kim, em que um
atacante anuncia o serviço 0x1001 (que é da VDE) e as vítimas passam a usá-lo?

**Montagem:** VDE verdadeira (.10), ADAS (.2) e CLU (.3) sobem e assinam a VDE. Depois de 15 s sobe
um clone da VDE com IP .11: mesmo binário, mesma entrada da matriz, outro unicast. A captura é lida
por `tools/attack_flows.py` com a regra do dataset.

```sh
bash platform/dup-offer/run.sh                      # oferta a cada 20 ms, como o DoS do dataset
CLONE_CYCLE_MS=2000 bash platform/dup-offer/run.sh  # ritmo de uma ECU legítima
```

## O que o dataset mostra (tools/attack_flows.py, tools/offer_cadence.py, tools/sd_detail.py)

| | DoS (.11) | MITM single (.13) | MITM multi (.14 + .15) |
|---|---|---|---|
| ritmo da oferta falsa de 0x1001 | 49/s (20 ms) | 98/s (10 ms) | normal (2 s), pelo .15 |
| `StopOffer` em nome da VDE, direto à ADAS | não | 357, do IP do .13 | **100 930, com o IP da VDE forjado** (MAC do .15) |
| quem abre o TCP com o atacante | ADAS, CLU, PT, TEL (~9 800 vezes cada) | ADAS | ADAS |

O IP forjado é visível pelo MAC: `02:42:ac:12:00:0f` (o .15) envia quadros com origem .10 e .15.
**A regra de rótulo do dataset (origem fora das 9 ECUs) marca esses StopOffer como normais.**

## O que a bancada mostrou

| | ritmo normal (2 s) | ritmo do DoS (20 ms) |
|---|---|---|
| ofertas do clone | 26 | ~2 100 (47/s) |
| vítimas assinam no clone | sim, 25 vezes cada | sim, 2 087 vezes cada |
| vítimas abrem TCP com o clone | **não** | **não** |
| conexão TCP com a VDE derrubada e reaberta | 27 vezes | **2 059 vezes** (ADAS) |
| notificações recebidas pela ADAS | 5 030 | **1 620** |
| notificações recebidas pelo CLU | 3 340 | **1 067** |

Os logs do vsomeip dizem por quê. Na vítima, o endpoint do serviço alterna
(`Reliable service endpoint has changed: old .10 new .11`, e volta), mas a conexão TCP segue
sendo reaberta com a VDE. No clone, cada assinatura chega sem TCP e é recusada
(`Subscription rejected ... didn't find TCP connection`).

## Versões e ordem de subida (02/11, segunda rodada)

O dataset foi capturado em 2025-08-08 (carimbo do primeiro pacote). Repetido com as versões que
existiam na época:

| versão | vítimas abrem TCP com o clone (clone entrando depois) |
|---|---|
| 3.6.4 | não |
| 3.5.6 | não (355 reconexões com a VDE por vítima) |
| 3.1.20.3 | quase nada (5 SYN da ADAS, 2 do CLU) |
| 3.4.10 | não compila com o GCC do Ubuntu 24.04; não foi preciso |

**A causa não era a versão, era a ordem de subida.** `tools/attack_timeline.py` sobre o PCAP do DoS
mostra as vítimas procurando o serviço em 0,08 s, o .11 respondendo, e a VDE só oferecendo em
3,8 s. Com `CLONE_FIRST=1` (clone no ar, vítimas sobem, VDE 4 s depois), **o 3.6.4 reproduz o
dataset na mesma sequência**: Find → TCP com o clone → Subscribe/Ack; a VDE chega, as vítimas tentam
assinar nela e recebem `SubNack` (sem TCP); zero SYN para a VDE; CLU e ADAS recebem só do clone.

```sh
CLONE_FIRST=1 bash platform/dup-offer/run.sh
```

Nos outros ataques a ordem é outra:

| ataque | atacante sobe | como pega a vítima |
|---|---|---|
| DoS (.11) | antes da VDE | ganha a corrida da subida |
| MITM single (.13) | 3 s depois da VDE | CLU, PT e TEL tentam assinar nele e desistem (o mesmo que a primeira rodada deste degrau); a ADAS cai pelo `StopOffer` em nome da VDE |
| MITM multi (.14, .15) | .14 antes (assina a VDE), .15 depois | `StopOffer` com IP da VDE forjado, à ADAS |
| Fuzzy (.17) | 1,2 s | não tem vítima |

## Leitura

- **O vsomeip 3.6.4 aceita a segunda oferta e as vítimas assinam nela.** O risco 1 da fase A0 não
  bloqueia a construção dos atacantes.
- **Quando o atacante entra depois da VDE, a oferta falsa derruba a conexão legítima em vez de
  sequestrá-la.** É um DoS real (a ADAS perde dois terços das notificações), e é o que acontece com
  CLU, PT e TEL no MITM single do dataset. Quando ele já está no ar antes da VDE, as vítimas migram
  para ele, como no DoS do dataset.
- ~~A hipótese mais provável é versão do vsomeip~~: refutada pela segunda rodada. A diferença era a
  ordem de subida; o 3.6.4 reproduz o sequestro quando o atacante está no ar antes da VDE.
- **O `StopOffer` em nome da VDE** (o que faz só a ADAS cair nos dois MITM) não sai da API do
  vsomeip: o atacante vivo precisa de um emissor de SD bruto, com IP forjado no MITM multi.
- **Na bancada, o rótulo deve ser pelo MAC do container atacante**, não pelo IP de origem, para
  que os quadros com IP forjado sejam rotulados como ataque.
