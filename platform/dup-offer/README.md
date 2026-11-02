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

## Leitura

- **O vsomeip 3.6.4 aceita a segunda oferta e as vítimas assinam nela.** O risco 1 da fase A0 não
  bloqueia a construção dos atacantes.
- **Mas, nesta versão, a oferta falsa derruba a conexão legítima em vez de sequestrá-la.** É um DoS
  real (a ADAS perde dois terços das notificações), mas não é o mesmo efeito do dataset, em que as
  vítimas migram para o atacante.
- A hipótese mais provável é **versão do vsomeip** diferente da usada pelo Kim, que o artigo não
  informa. O próximo passo é repetir este degrau com versões anteriores.
- **O `StopOffer` em nome da VDE** (o que faz só a ADAS cair nos dois MITM) não sai da API do
  vsomeip: o atacante vivo precisa de um emissor de SD bruto, com IP forjado no MITM multi.
- **Na bancada, o rótulo deve ser pelo MAC do container atacante**, não pelo IP de origem, para
  que os quadros com IP forjado sejam rotulados como ataque.
