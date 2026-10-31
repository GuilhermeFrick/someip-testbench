# Fase 0 fechada: dois nós nossos trocando SOME/IP

```bash
python tools/gen_config.py --out build/cfg    # pré-requisito
bash platform/node-handshake/run.sh
```

O [`sd-smoke`](../sd-smoke/) provou que o Service Discovery funciona entre
containers, usando os **binários de exemplo** do vsomeip. Este degrau prova o
mesmo com a **nossa fachada** e a **configuração gerada da matriz**, que é o
critério de pronto da Fase 0.

## Resultado

Passou. 22 notificações recebidas em 20 segundos, e os campos no barramento batem
com a matriz:

```
[gnss_ecu] registered with the routing manager
[gnss_ecu] service offered; publishing Position every 1000 ms
[hmi_ecu]  service found through Service Discovery
[hmi_ecu]  subscribed to eventgroup 0x4455
[hmi_ecu]  notification 1 of event 0x8777, 8 bytes, counter=4

SOMEIP, service 4660, event 1911, len 16, msgtype NOTIFICATION, retcode E_OK
```

| Campo no barramento | Valor | Vem de |
|---|---|---|
| `service 4660` | 0x1234 | `GnssService::Id()` |
| `event 1911` | 0x8777 | `GnssService::Events::Position()` |
| `msgtype` | NOTIFICATION | evento, não request |
| `len 16` | 8 de payload + 8 | nossa carga de 8 bytes |

E os dois lados concordam no conteúdo: o provedor enviou o contador 4, o
consumidor leu `counter=4`, e o hex da captura mostra `0000 0004 dead beef`.

## O que este degrau prova, que o anterior não provava

1. **A fachada `libecu` funciona**, não só a pilha. Os dois nós usam
   `Skeleton`/`Proxy` e não contêm uma linha de vsomeip.
2. **O gerador fecha o círculo.** Nenhum identificador foi escrito à mão: serviço,
   instância, evento e eventgroup vêm do `build/cfg/CommMatrix.hpp`, e a
   configuração da pilha vem do mesmo YAML. Se os dois divergissem, o SD não
   fecharia.
3. **A ordem que o vsomeip exige está escondida na fachada.** `offer_service` só é
   aceito depois de `ST_REGISTERED`, e `subscribe` só depois de o serviço estar
   disponível. Os nós não sabem disso: chamam `Offer()` e `Subscribe()` e a
   fachada resolve.

## Por que o `-T someip` no veredito

Na porta do serviço (30509) o `tcpdump` **não** adivinha SOME/IP e mostra apenas
`UDP, length 24`; ele só reconhece sozinho a 30490 do Service Discovery. Com
`-T someip` os campos aparecem, e é assim que se confere que `service` e `event`
batem com a matriz. Sem isso o teste "passaria" sem provar que o quadro está certo.

## Requisitos de ambiente, herdados do sd-smoke

- **rota multicast** `224.0.0.0/4` em cada nó, criada **antes** de a pilha subir, e
  portanto `--cap-add NET_ADMIN`. Sem ela o vsomeip não liga o SD e falha em
  silêncio. O `netlink: ... mc=1` no log é o sinal de que a rota foi vista.
- **captura no container do provedor.** Um monitor passivo em container separado
  não vê o multicast, porque a bridge do Docker só entrega a quem entrou no grupo
  por IGMP.

## Armadilha que este teste pegou, e vale registrar

Ao traduzir a saída dos nós para inglês, o veredito aqui continuou procurando a
palavra `notificacao`. O teste passava, as notificações chegavam, e o script
dizia **FALHOU**. Hoje ele procura `notification [0-9]`, e o comentário ao lado
explica por que as palavras procuradas estão em inglês enquanto o script está em
português: **o código é em inglês, a documentação de bancada é em português.**
