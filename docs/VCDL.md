# vCDL: a matriz da bancada no formato do CANoe

O `tools/emit_vcdl.py` escreve a matriz de comunicação em **vCDL** (*Vector
Communication Description Language*), o formato que o Vector CANoe carrega como
*Data Source*. A mesma matriz que sobe os nós em vsomeip pode então ser aberta na
ferramenta comercial de referência — **um barramento, duas implementações**.

Isto não é dependência: é interoperabilidade. O argumento da segunda contribuição
continua sendo que a cadeia de referência é proprietária e isso limita a
reprodutibilidade. O que muda é que agora a nossa matriz **atravessa** essa
fronteira em vez de ignorá-la, e isso abre a validação mais forte que a bancada
pode ter: montar o mesmo barramento nos dois, exportar PCAP de cada um e comparar.

## ⚠️ O que foi verificado e o que não foi

A gramática abaixo foi **lida**, não adivinhada: vem do manual da Vector
versionado em `referencias/` (Part 2, slides 7 a 13) e da ajuda online do CANoe.
A arquitetura foi confirmada por observação direta numa sessão do **CANoe 20
(SP2) /pro**, onde também se verificou que:

* o barramento totalmente simulado (`[Simulated Bus]`) **não precisa de hardware**;
* e ainda assim produz quadros fiéis — uma oferta de SD aparece como
  `Offer Service | Calculator | Version 0.1 | Instance 1 | 192.168.1.1:9C40 | UDP`;
* e a captura exporta em **PCAP** e **PCAPNG**.

🔴 **Mas a saída do emissor NUNCA foi carregada num CANoe real.** A sessão de
demonstração onde a gramática foi lida bloqueia transferência de arquivos, então
não houve como validar. Em particular, **um atributo é inferência**: o manual só
mostra `udpEndpoint`, e `tcpEndpoint` é o nome óbvio do par — mas é palpite, e
está marcado como tal no código. Quem tiver uma licença, comece por aí.

## A gramática, em resumo

**Ajustes globais** — Service Discovery e serialização, seguidos de `binding SOMEIP;`:

```
[ offerTTL=3s, offerCycleTime=2s, alignment=4, serializationByteOrder="Motorola" ]
binding SOMEIP;
```

**Tipos e interface de serviço** — `fixlayout=true` dá ao struct layout binário
fixo, sem campo de comprimento na frente, que é o que o payload cru do barramento é:

```
namespace DataTypes
{
    [fixlayout=true]
    struct Payload24 { array<uint8, 24> data; }

    interface Service1001
    {
        event Payload24 Event0001;
    }
}
```

**Endpoints** — três camadas, nesta ordem: pilha IP (com MAC), *network endpoint*
(endereço), *application endpoint* (porta):

```
namespace NetworkEndPoints
{
    ipstack node_10 { [mac="02:00:AC:12:00:0A"] ethernetadapter ETH { } }
    ipnep  NEP_node_10   [ethernetAdapter=node_10.ETH, address="172.18.0.10", prefix=24];
    ipnep  NEPSD_node_10 [ethernetAdapter=node_10.ETH, address="224.244.224.245"];
    ipaep  AEPSD_node_10 [nep=NEPSD_node_10, port=30490];
    ipaep  AEP_node_10_Service1001 [nep=NEP_node_10, port=30501];
}
```

**Binding SOME/IP** — identificadores, eventgroups e quem provê e quem consome:

```
namespace CommunicationObjects
{
    [serviceId=0x1001, version=0]
    service Service1001 : DataTypes::Service1001
    {
        [eventId=0x0001]
        refine Event0001;

        binding SOMEIP
        {
            [groupId=0x0001, multicastThreshold=0]
            eventgroup EG0001 = Event0001;
        }

        [instanceId=0x0001, tcpEndpoint=NetworkEndPoints::AEP_node_10_Service1001,
         sdMulticastEndpoint=NetworkEndPoints::AEPSD_node_10]
        provider node_10;

        [clientId=0, tcpEndpoint=NetworkEndPoints::AEP_node_2_Service1001,
         sdMulticastEndpoint=NetworkEndPoints::AEPSD_node_2]
        consumer node_2;
    }
}
```

**Participantes** — a ECU que agrupa os endpoints:

```
namespace Participants
{
    participant node_10 { CommunicationObjects::Service1001.node_10 }
}
```

## O mapeamento, campo a campo

| matriz da bancada | vCDL |
|---|---|
| `services.<nome>.id` | `[serviceId=0x....]` |
| `services.<nome>.instance` | `[instanceId=0x....]` no `provider` |
| `events[].id` | `[eventId=0x....]` antes do `refine` |
| `events[].payload_bytes` | `struct Payload<N> { array<uint8, N> data; }` |
| `eventgroups[].id` / `.events` | `[groupId=...] eventgroup EG.... = ...;` |
| `nodes.<n>.ip` | `ipnep [address="..."]` |
| `nodes.<n>.provides` | `provider <n>;` |
| `nodes.<n>.consumes` | `consumer <n>;` |
| `services.<nome>.protocol` | `tcpEndpoint` × `udpEndpoint` ⚠️ |
| `network.service_discovery.*` | atributos globais |

Duas coisas a matriz **não** tem e o vCDL exige:

* **MAC** — derivado do IP (`mac_de()`), porque endereço de enlace não entra em
  nenhuma das 16 features e o `extract_matrix.py` não o recupera;
* **parâmetros de serialização** — os do exemplo da Vector, porque a captura não
  os expõe e inventá-los seria pior que copiá-los.

## Como gerar

```sh
python3 tools/emit_vcdl.py --matrix platform/comm_matrix_kim.yaml --out build/cfg/kim.vCDL
```

A matriz recuperada de 9 nós dá 353 linhas; a da bancada, 126.

## O passo que falta

Carregar num CANoe real e corrigir o que ele recusar. O CANoe valida com código e
posição (`KimBus.vCDL (L7,C9): [VL0001] Parser...`), então o ciclo é rápido para
quem tiver licença — e cada erro corrigido vale para as duas matrizes de uma vez.
