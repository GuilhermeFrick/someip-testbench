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

## ✅ VALIDADO contra o parser do CANoe 20 (SP2)

A saída do emissor foi carregada num CANoe real e o parser a aceita. O caminho até
lá rendeu três correções, e a primeira é a que explica as outras duas.

### 🔴 A sintaxe do manual é vCDL **1.4**, não 2.0

Este foi o erro de origem. A página de ajuda **"SOME/IP Binding"** da Vector fica
sob **`CANoe » vCDL » vCDL 1.4 (deprecated) » Language`**. Ou seja: tudo o que o
manual Part 2 documenta — `ipstack`, `ipnep`, `ipaep`, `provider`, `consumer`,
`participant`, `binding SOMEIP;` e os atributos em minúscula — é **1.4**.

Declarando `version 2.0;` o parser recusa tudo isso de uma vez, com cerca de 40
erros `[VL1001] Artifact or subelement ... not found`. E ele diz o porquê:

> `(L1,C1) [IMP1001] Remember all predefined attributes in vCDL 2.0 start with an uppercase letter`

Com **`version 1.4;`** todos esses erros desaparecem.

O caminho para o 2.0 existe e está anotado para depois: o exemplo `ADASSystem_Setup.vcdl`
usa `import module "ADAS";` com `[Binding="ADAS"]` e nomes qualificados
`_ADAS::DataModel::ISensor`. É outra tradução, não um ajuste.

### A ordem das duas primeiras linhas importa

`version` tem de ser a **primeira** linha. Com o `import` antes dela:

> `KimBus.vCDL: The vCDL file version could not be found`

### O módulo chama-se `SOMEIP` — confirmado por controle

Não por dedução: trocando por um nome inventado, o parser avisa

> `(L2,C15) [COE0119] No module with this name can be found and thus no binding will be activated.`

e esse aviso **nunca** aparece com `"SOMEIP"`.

### `tcpEndpoint` é válido — também por controle

Com `tcpEndpoint`, nenhum erro. Com `xyzEndpoint` inventado, as duas linhas são
rejeitadas:

> `(L39,C21) [VL1013] Attribute refers to neither an attribute definition nor an attribute bundle`

É isso que prova que o parser de fato confere o nome do atributo — sem o controle,
"nenhum erro" não significaria nada.

### O único resíduo: `[fixlayout=true]`

Depois das correções sobrou um erro só, no atributo que o manual mostra antes do
struct:

> `(L7,C2) [VL1013] Attribute refers to neither an attribute definition nor an attribute bundle`

O emissor **deixou de emiti-lo**. O mesmo efeito — layout binário fixo, sem campo
de comprimento — vem de `structLengthFieldSize=0`, que já está nos atributos
globais. ⚠️ Essa equivalência é raciocínio, não medição: ninguém comparou os bytes
no fio com e sem o atributo.

### Notas de operação da sessão de demonstração

* O diálogo **Load Data Source** só valida de verdade **depois do `Finish`** — a
  primeira tela quase não mostra nada;
* ele às vezes abre **invisível**; `Alt+Space` o traz à frente, `Esc` fecha;
* atalhos de `Ctrl` **não chegam** à sessão: `Ctrl+X` digita um "x".

## O passo que falta

O arquivo carrega. O que **não** foi feito é **rodar** a simulação a partir dele e
comparar o PCAP com o da nossa bancada — a sessão de demonstração não deixa arquivo
sair, então a comparação cruzada continua esperando uma licença.

E há uma dívida anotada: migrar para **vCDL 2.0**, que renomeia os atributos para
maiúscula inicial e usa `[Binding="SOMEIP"]` em vez de `binding SOMEIP;`. Não é
urgente — o 1.4 é o que o próprio manual da Vector documenta para SOME/IP — mas
está marcado como *deprecated* na ajuda.
