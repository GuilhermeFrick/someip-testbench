# O sensor no barramento, e o que ele revelou

```bash
python3 tools/gen_config.py --out build/cfg                    # pré-requisitos
bash tools/build_sensor_artefacts.sh <dir-dos-artefatos-do-treino>
bash platform/sensor-live/run.sh
```

O [`node-handshake`](../node-handshake/README.md) provou que os nós trocam SOME/IP. Este prova que
o `ids_sensor` **captura esse tráfego no barramento**, classifica cada quadro com o **modelo
adotado** e reporta ao IdsM.

## Passou, e o mecanismo funciona

```
[ids_sensor] joined 224.244.224.245:30490  (Service Discovery)
[ids_sensor] joined 224.225.226.233:32344  (eventgroup 0x4465 of GnssService)
[ids_sensor] inference engine: compacto
[ids_sensor] packets          : 43
[ids_sensor] dropped by parser: 80
[ids_sensor] security events  : 28
```

Os dois grupos multicast foram assinados, que é o que faz o Service Discovery chegar a este
container — sem isso a captura pareceria saudável e faltaria uma camada inteira do tráfego.

O motor ativo é o **compacto** porque o `run.sh` não linka o C99 gerado ao nó; o nó prefere o
TL2cgen quando ele está presente e **diz qual está usando**, porque uma medição tomada no motor
errado é pior do que nenhuma medição.

Os 80 quadros descartados pelo parser são ARP, IPv6 e o que mais a bridge do Docker emite. São
quase o dobro dos aceitos, e é por isso que o `Sensor` os conta **à parte**: misturá-los puxaria
para baixo as médias dos outros três estágios na proporção do lixo que a rede por acaso carrega.

## 🔴 Mas ~30 eventos em ~44 pacotes de tráfego BENIGNO

Esse é o resultado que importa, e ele não é defeito de código. **O modelo foi treinado no dataset
do Kim e está vendo o barramento da nossa bancada, que tem outra distribuição.**

Os totais variam de execução para execução, porque o volume de ruído da bridge varia (28 a 33
eventos em 43 a 44 pacotes, nas execuções feitas). **A repartição, não.** Uma execução, aberta por
origem e serviço:

| origem | eventos | serviço/método | o que é |
|---|---:|---|---|
| `172.30.0.11` (gnss_ecu) | 15 | `0xFFFF / 0x8100` | **Service Discovery OfferService** do nosso próprio provedor |
| `172.30.0.1` (gateway da bridge) | 13 | `0x0000 / 0x0000` e `0x0000 / 0x8400` | tráfego do Docker que **não é SOME/IP** |

O `run.sh` faz essa repartição sozinho, pelo
[`resumo_eventos.py`](resumo_eventos.py) — a contagem por classe isolada engana, porque esconde que
os eventos vêm de origens diferentes por motivos diferentes.

Duas causas distintas, e cada uma diz uma coisa.

**1. Metade dos eventos nem vem da bancada.** São quadros do gateway da bridge do Docker. O parser
os aceita porque reproduz fielmente a referência, que trata **qualquer payload UDP de 16 bytes ou
mais como um cabeçalho SOME/IP** — daí o `service_id = 0x0000`. É a mesma família de comportamento
do *padding* Ethernet documentada em [`parity-dataset`](../parity-dataset/README.md): a referência
inventa mensagens SOME/IP a partir de bytes que não são SOME/IP. No dataset isso era inofensivo
porque a captura só tinha o barramento; num barramento real, com ruído de infraestrutura, vira
detecção.

**2. A outra metade são os nossos próprios anúncios de serviço.** Todo `OfferService` do
`gnss_ecu` foi classificado como MITM-multi. Faz sentido: as assinaturas de MITM no dataset do Kim
envolvem SD (*withdraw* com TTL=0, serviço de relay), e os nossos identificadores de serviço
(`0x1234`) não estão no vocabulário em que os modelos de bytes foram ajustados. Bytes fora do
vocabulário dão entropia cruzada alta, que é justamente a região onde moram as classes de ataque.

**As confianças confirmam a leitura:** mínimo 0,51, mediana entre **0,52 e 0,60**, máximo 0,95.
Com cinco classes, 0,52 é pouco acima do nada. O modelo não está confiante — está extrapolando.

### O que isso significa, e o que NÃO significa

Não significa que o IDS é ruim: no dataset em que foi treinado e avaliado ele tem macro-F1 0,9689.
Significa que **ele não transfere para outro barramento sem recalibração**, e que medir isso exige
exatamente uma bancada como esta. É o argumento que justifica a Fase 1: tráfego benigno ancorado no
CARLA, gerado *neste* barramento, para reajustar os modelos de bytes e os limites.

Também é um bom exemplo de divisão de responsabilidade: **o sensor não filtra**, de propósito. Com
confiança mediana de 0,60, uma cadeia de filtros do IdsM eliminaria a maior parte desses eventos —
e é no IdsM que essa decisão pertence, não no sensor.

## ⚠️ Os tempos desta execução NÃO são medida de desempenho

```
per packet, us : parse 2.124  features 13.271  classify 64.088  decide 121.631
```

Três motivos para não citar esses números:

1. **são 44 pacotes.** A média por pacote é dominada por partida a frio: primeiras faltas de
   página, cache vazia;
2. **o `decide` mede o sink da bancada, não a decisão.** O `ara::idsm` desta bancada abre e fecha
   um arquivo **por evento**, o que está documentado como a troca certa para uma bancada e errada
   para um caminho quente. Com 33 eventos em 44 pacotes, são ~162 µs por evento de abrir, escrever
   e fechar. O IdsM de verdade é IPC, e não se parece com isso;
3. **cinco leituras de relógio por quadro** entram na conta, como o `Sensor.cpp` registra.

A medida limpa é a do banco de latência (1,39 µs por inferência com o modelo adotado). Este degrau
prova **função**, não desempenho.

## O que ele exercita que nenhum teste exercitava

- a entrada nos grupos multicast por IGMP, que é a diferença entre ver e não ver o Service
  Discovery;
- a cadeia completa com os artefatos **reais** do treino, não com um modelo de prova;
- o `ara::idsm` resolvendo contra a lista de eventos **gerada da matriz de comunicação**;
- a `LiveSource` num barramento de verdade, com o laço de captura viva, que não é o
  `Sensor::Run()`.

## Erro que este degrau pegou, na primeira execução

O `docker kill --signal=SIGINT` manda o sinal para o **PID 1 do container**. Como o container subia
com `sh -c "...; ./build/bin/ids_sensor_node"`, o PID 1 era o `sh`, que **não repassa** o sinal ao
filho: o nó morria sem imprimir o resumo, e o veredito lia zero pacote tendo o sensor trabalhado os
25 segundos inteiros. O `exec` resolve, porque põe o nó como PID 1.

O nó também passou a usar saída em **modo linha**: num pipe, como o do `docker logs`, a libc usa
buffer cheio e a telemetria de progresso não aparecia enquanto seria útil.
