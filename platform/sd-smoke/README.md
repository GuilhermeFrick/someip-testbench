# Teste de fumaça do Service Discovery

Degrau 0b do [plano](../../docs/PLANO.md): confirmar que o **SOME/IP-SD anuncia e
descobre** entre containers, com os exemplos do próprio vsomeip, **antes** de existir
qualquer nó nosso.

```bash
bash run.sh
```

## Resultado

Passou. O serviço anuncia por SD, o cliente descobre, assina e recebe notificações:

```
netlink: from 3 to 4, if=2, mc=1, count=1
172.30.0.11.30490 > 224.244.224.245.30490: SOMEIP, service 65535, ... NOTIFICATION
TOTAL: 80
Received a notification for Event [1234.5678.8778] = (9) 00 01 02 03 04 05 06 07 08
```

O `tcpdump` decodifica como **SOMEIP** nativamente, e `service 65535` (`0xFFFF`) é o
identificador de Service Discovery.

## O achado: o Docker não cria rota multicast

Esta é a parte que custou a depuração, e vira **requisito de arquitetura**.

O vsomeip só liga o Service Discovery depois de observar, via netlink, que existe
**interface no ar E rota multicast**. O Docker cria apenas:

```
default via 172.30.0.1 dev eth0
172.30.0.0/24 dev eth0 proto kernel scope link src 172.30.0.11
```

Não há rota para `224.0.0.0/4`. Sem ela, o vsomeip entra num laço silencioso:

```
Added service 0x1234 to pending_sd_offers[1]
Removed service 0x1234 from pending_sd_offers[0]    (10s depois)
Added service 0x1234 to pending_sd_offers[1]        (10s depois)
```

**Sem erro, sem aviso, sem pista óbvia.** O log diz "Service Discovery module loaded"
e a interface aparece como *up*; tudo parece correto. O único sinal é o campo `mc`
do netlink preso em zero.

A correção é uma linha por nó, aplicada **antes** de o vsomeip subir:

```bash
ip route add 224.0.0.0/4 dev eth0
```

Isso exige `--cap-add NET_ADMIN` em cada container.

## Dois requisitos que isso impõe à bancada

1. **Todo nó ECU precisa da rota multicast e de `NET_ADMIN`.** Não é detalhe de
   ambiente: sem isso o SD não existe, e a bancada inteira depende dele.
2. **Um monitor passivo em container separado NÃO captura o multicast.** A bridge
   do Docker só entrega a quem entrou no grupo por IGMP, e um observador passivo
   não entra. A primeira tentativa capturou zero pacotes de SD por esse motivo, o
   que me levou a culpar o vsomeip antes da hora. O monitor da bancada terá de
   entrar nos grupos ou usar espelhamento de porta.

## Por que o diagnóstico demorou

O caminho de depuração foi do mais barato ao mais caro, e isso importou:

| Teste | Resultado | O que eliminou |
|---|---|---|
| 0a, multicast cru entre containers | passou | "Docker não faz multicast" |
| Config própria | 0 pacotes | — |
| Config de referência do vsomeip | 0 pacotes | "minha config está errada" |
| vsomeip 3.6.4 em vez de master | 0 pacotes | "regressão no master" |
| Netlink com `mc=0` | **causa** | — |

Como o 0a já havia provado que multicast funcionava, sobrou procurar dentro do
vsomeip. Se o primeiro passo tivesse sido escrever o `comm_matrix.yaml` e os nós,
essa depuração aconteceria no meio de código nosso, com uma variável a mais.

## Nota sobre as configurações

`service.json` e `client.json` vêm das configs de referência do vsomeip
(`vsomeip-udp-service.json` e `vsomeip-udp-client.json`), com apenas o `unicast`
ajustado para os IPs fixos da bancada.

Uma tentativa anterior, escrita à mão, omitia `events` e `eventgroups` no serviço.
Não foi a causa da falha, mas usar a referência elimina essa classe de erro.
