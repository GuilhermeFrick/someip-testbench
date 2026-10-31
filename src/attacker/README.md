# `attacker` — o nó atacante

Fase 2 do [plano](../../docs/PLANO.md). Um executável com **duas superfícies de
ataque**, que não são intercambiáveis:

| Superfície | Como age | Ataques |
|---|---|---|
| **via serviço** | usa a `libecu` como um nó legítimo, com a pilha vsomeip | MITM, spoof de Service Discovery |
| **on-the-wire** | monta o quadro direto no socket, sem passar pela pilha | DoS, Fuzzy |

A distinção importa. Um ataque *via serviço* produz pacotes bem formados, que a
pilha aceita, e por isso é o caso difícil para o detector. Um ataque
*on-the-wire* pode violar a serialização de propósito, e é o único jeito de gerar
Fuzzy, porque a pilha não deixaria.

## Cenários

Em [`platform/scenarios/`](../../platform/scenarios/), em YAML, herdando a
taxonomia e o `trigger_rate` do `someip-traffic-simulator`.

## Ground truth

A rotulagem é por **IP de origem mais timeline**, via `tools/label_pcap.py`. Por
isso o endereço de cada nó é contrato na
[matriz de comunicação](../../platform/comm_matrix.yaml), não configuração.

## Risco conhecido

Viés sintético: ataque com padrão temporal perfeito demais fica trivial de
detectar e o resultado não se transfere. Mitigação prevista no gerador: jitter e
variação de taxa.

## Critério de pronto

O extrator `content_ext` consome o PCAP gerado **sem adaptação**, e o detector da
primeira contribuição produz métricas por classe sobre ele.
