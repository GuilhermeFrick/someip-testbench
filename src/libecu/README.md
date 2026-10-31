# `libecu` — fachada estilo `ara::com` sobre vsomeip

**É o próximo passo do projeto.** Ver [`docs/PLANO.md`](../../docs/PLANO.md), Fase 0.

## O que vai aqui

Uma camada fina que esconde a API do vsomeip atrás de dois conceitos do
AUTOSAR Adaptive, para os nós serem escritos no vocabulário da norma e não no da
biblioteca:

| Conceito | Papel |
|---|---|
| `Skeleton` | lado provedor: oferece o serviço, publica `Event` e `Field`, atende `Method` |
| `Proxy` | lado consumidor: encontra o serviço, assina `eventgroup`, recebe notificação |

A fachada não é enfeite. Ela é o que permite que o nó não saiba nada sobre
`request_service`, `register_availability_handler` ou o ciclo de vida do
`application` do vsomeip, e é o que torna os nós comparáveis a código de ECU real.

## O que ela consome

Os identificadores vêm da matriz de comunicação, nunca escritos à mão:

```
platform/comm_matrix.yaml  ->  tools/gen_config.py  ->  config vsomeip + headers C++
```

**Pendência conhecida:** o `gen_config.py` já gera a config da pilha e o mapa de
rótulos, mas **ainda não gera os cabeçalhos C++** com os identificadores. Isso
precisa sair antes ou junto da `libecu`, senão os IDs voltam a ser literais
espalhados pelo código, que é exatamente o que a matriz existe para evitar.

## Dois requisitos de ambiente, já descobertos

Vêm do teste de fumaça do SD, em [`platform/sd-smoke/README.md`](../../platform/sd-smoke/README.md):

1. Cada nó precisa de `ip route add 224.0.0.0/4 dev eth0` **antes** de o vsomeip
   subir, e portanto de `--cap-add NET_ADMIN`. Sem a rota, o vsomeip não liga o
   Service Discovery, e falha **em silêncio**.
2. Um monitor passivo em container separado não captura o multicast: a bridge do
   Docker só entrega a quem entrou no grupo por IGMP.

## Critério de pronto

Os identificadores da matriz coincidem de propósito com os dos exemplos do
vsomeip. Então o teste é por substituição:

> dois nós nossos, gerados a partir da matriz, ocupam o lugar do `notify-sample` e
> do `subscribe-sample`, e o dissector do Wireshark continua decodificando o
> SOME/IP-SD e as notificações corretamente.

Se passar, a fachada está certa e o gerador está certo, cada um conferido contra
um binário conhecidamente correto.
