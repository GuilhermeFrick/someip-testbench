# Testes

Rodam no **host**, nunca no alvo, como a §10 do padrão de código exige.

```bash
# completo: a imagem da bancada tem vsomeip E libpcap
docker run --rm -v "$PWD":/w -w /w someip-base:3.6.4 bash tests/run_all.sh

# no host cru: pula o que depender de biblioteca ausente, e DIZ o que pulou
bash tests/run_all.sh
```

Tudo com os sanitizers ligados, menos o `test_libecu`, que linka o vsomeip e roda sem eles.

Nada aparece como aprovado por omissão. Cada conjunto que não pode rodar imprime **PULADO** com o
motivo e o que falta instalar.

| Arquivo | Tipo | O que verifica |
|---|---|---|
| `test_ara_core.cpp` | unitário | o subconjunto [`src/ara`](../src/ara/), incluindo `Result<void>` |
| `test_bytemodel.cpp` | unitário | `HammingDistance`, a pontuação **posicional** e os erros de carga do artefato |
| `test_feature_streamer.cpp` | unitário | validação, `Reset`, isolamento das janelas e o **despejo** |
| `test_libecu.cpp` | unitário | os **caminhos de erro** da fachada, que o handshake não toca |
| `test_packet_source.cpp` | unitário | códigos de erro, fim de arquivo e o **registro truncado** (precisa de `libpcap-dev`) |
| `test_normalizer.cpp` | unitário | a **feature degenerada** e a **ausência de saturação**, as duas coisas que o min-max "normalmente" faz e a referência não |
| `test_event_dispatch.cpp` | unitário | serialização do contexto, validação do caminho do meta-modelo e **um reporter por classe** |
| `test_classifier.cpp` | oráculo | os **dois motores** contra o `predict` do próprio XGBoost |
| `test_sensor.cpp` | integração | a **contabilidade da cadeia**: descarte à parte, evento, relógio do cenário |
| `test_comm_matrix.cpp` + `check_comm_matrix.py` | consistência | o cabeçalho gerado contra o YAML da matriz |
| `test_parity_parser.cpp` + `gen_parser_vectors.py` | oráculo | **o parser campo a campo contra o `someip.parse` do Python** |
| `test_parity_streamer.cpp` + `gen_parity_vectors.py` | oráculo | **as 16 features contra o extrator do Python** |
| [`platform/node-handshake`](../platform/node-handshake/) | integração | dois nós trocando SOME/IP em containers |
| [`platform/parity-dataset`](../platform/parity-dataset/) | oráculo, **dataset inteiro** | a cadeia de produção inteira sobre os 7 PCAPs reais |

**352 asserções** numa só invocação, todas sob AddressSanitizer e UndefinedBehaviorSanitizer (menos
o `test_libecu`), mais o `test_comm_matrix` (21 identificadores conferidos contra o YAML pelo lado
Python) e o `test_parity_streamer` (368 comparações). Por conjunto:

| conjunto | asserções |
|---|---:|
| `test_ara_core` | 57 |
| `test_sliding_window` | 45 |
| `test_feature_streamer` | 39 |
| `test_bytemodel` | 38 |
| `test_event_dispatch` | 37 |
| `test_packet_source` | 29 |
| `test_normalizer` | 28 |
| `test_libecu` | 27 |
| `test_sensor` | 21 |
| `test_classifier` | 17 |
| `test_parity_parser` | 14 |

Duas asserções do `test_bytemodel` **não rodam como root**: a do `ArtefactUnreadable` depende de
`chmod 0` barrar a leitura, e para root não barra. O teste imprime `(pulado: ... root?)` em vez de
passar em falso, então fora de container o total é 354.

## O levantamento que deu origem aos unitários

Até aqui havia um teste de oráculo (paridade), um de integração (handshake), um de
consistência (matriz) e **um** unitário. Levantando a cobertura dos códigos de erro:

| | declarados | exercitados antes | agora |
|---|---:|---:|---:|
| `EcuErrc` (libecu) | 11 | **0** | 8 |
| `IdsSensorErrc` | 7 | 5 | 7 |
| `Result<void>` | — | **0** | sim |

⚠️ **E o levantamento achou algo pior que falta de teste: seis códigos declarados
que nada nunca retornava.** Um código de erro que ninguém produz é documentação, não
comportamento, e ler o `enum` como lista de garantias estaria errado.

| código | situação |
|---|---|
| `ArtefactUnreadable` | **corrigido**: o `@retval` prometia desde o início, mas o `fopen` só devolvia `ArtefactNotFound`. Agora distingue por `errno` e leva o errno no *support data* |
| `ServiceDiscoveryNotReady` | o cabeçalho dizia que ele "transforma o silêncio do SD num erro que o chamador tem de tratar". **Não transforma**: a `Skeleton` oferece e não verifica. Marcado como não produzido, e a frase corrigida |
| `TrainingRunMismatch` | nenhuma verificação cruzada de artefatos existe. Marcado |
| `SubscriptionTimeout` | `Proxy::Subscribe` não espera confirmação. Marcado |
| `MalformedPacket`, `ModelNotAvailable` | os `.cpp` que os produziriam não existem ainda. Marcados |

Cada um deles carrega agora `NOT YET PRODUCED` no próprio enumerador, com o motivo.

### Dois já foram fechados

| código | como passou a ser produzido |
|---|---|
| `MalformedPacket` | o `SomeipParser` o devolve para quadro que não é IPv4, protocolo que não é TCP/UDP, e quadro curto demais para os cabeçalhos que declara |
| `ModelNotAvailable` | o `Tl2cgenClassifier::Create` o devolve quando os **símbolos fracos** do C gerado resolvem para nulo, isto é, quando a build pulou o passo do TL2cgen. É o único lugar do projeto onde "a build está incompleta" é resposta de runtime em vez de erro de ligação, e o `test_classifier` verifica os dois lados: sem o C gerado dá `ModelNotAvailable`, com ele a forma do modelo é conferida e as predições batem |

Seguem marcados `TrainingRunMismatch` (nenhuma verificação cruzada de artefatos existe),
`SubscriptionTimeout` e `ServiceDiscoveryNotReady`.

## Casos que pegaram erro de verdade

Não são hipóteses: cada linha abaixo é um defeito que o teste encontrou.

| teste | o que pegou |
|---|---|
| `test_comm_matrix` | `static constexpr` membro de dado quebrava a **ligação** em C++14 quando passado por referência; a compilação passava limpa |
| `test_feature_streamer` | a minha expectativa da rampa de `repeat_rate` estava errada, não o código: a taxa é calculada **antes** do append |
| `test_bytemodel` | o `@retval` documentava `ArtefactUnreadable` que a implementação não produzia |
| `run_all.sh` | `test_ara_core` estava **obsoleto** contra a API nova do `ByteModel` e nem compilava; passava antes porque eu o rodei antes da mudança |
| sensibilidade da paridade | o gerador não cobria a assimetria do payload vazio, e quebrar a janela de repetição **passava** |

```bash
cd <raiz do repo>
g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -fsanitize=address,undefined \
    -o /tmp/t tests/test_ara_core.cpp && /tmp/t
```

## Por que este teste existe

`Result` e `Optional` são template. **Cabeçalho compilar não prova nada**: os membros
de um template só são instanciados quando usados, e os dois guardam o valor em
`aligned_storage` com placement new. O erro típico ali, destrutor que não roda ou que
roda duas vezes, só aparece em execução.

Então o teste exercita de propósito os casos que quebram uma implementação ingênua:

- **tipo sem construtor padrão** (`ByteModels`), que elimina a implementação
  preguiçosa com um membro `T value;`;
- **tipo move-only** (`unique_ptr`), que exige o construtor de movimento correto;
- **contagem de construções e destruições**, para provar saldo zero ao sair do escopo;
- atribuição que **troca** valor por erro e erro por valor, onde o alternativa antiga
  precisa ser destruída antes de a nova ser construída;
- `subspan` com deslocamento e comprimento além do fim, que é onde uma vista estoura.

Roda sob **AddressSanitizer** e **UndefinedBehaviorSanitizer**: o primeiro pega
vazamento e duplo free, o segundo pega acesso desalinhado no `aligned_storage`.
Passa também com `-std=c++17`.

## O teste da fonte de pacotes, e o caso que o sanitizer não pega

As capturas de apoio são escritas pelo próprio teste, byte a byte, para o caso ficar visível ao
lado da expectativa. Ele cobre os códigos de erro (`ArtefactNotFound` para arquivo que não existe,
`ArtefactCorrupted` para arquivo que não é captura **e** para captura que não é Ethernet), o fim
de arquivo, a captura válida e vazia, e a interface de rede inexistente.

⚠️ **O caso que justifica o teste é o do registro truncado.** Um registro que no fio tinha 1514
bytes e foi capturado com 80: a vista tem de ter **80**. Trocar `caplen` por `len` foi verificado
e o teste acusa — mas o **AddressSanitizer não acusa**. O buffer do libpcap tem o tamanho do
`snaplen`, então os 1434 bytes a mais caem dentro da própria alocação dele e nenhuma ferramenta
reclama: o que sai são bytes de lixo tratados como payload, calados. É exatamente o tipo de erro
que a paridade sobre um dataset sem truncamento também não pegaria.

## Os motores de inferência, e a ABI que eu não supus

```bash
bash experiments/bench-inference/make_probe.sh   # pré-requisito
```

O `Tl2cgenClassifier` tem de declarar a `union Entry` e a assinatura de `predict` por conta
própria, para compilar com ou sem o passo do TL2cgen. **Errar o layout dessa união não dá erro de
compilação, dá corrupção silenciosa dos valores das features.** Então a ABI foi lida de um
`header.h` real, gerado rodando o TL2cgen sobre um modelo de prova da mesma forma do adotado:

```c
union Entry { int missing; float fvalue; int qvalue; };
void predict(union Entry *data, int pred_margin, float *result);
int32_t get_num_feature(void);
void get_num_class(int32_t *out);        /* escreve por ponteiro, NÃO retorna */
```

⚠️ **Duas dessas eu teria errado supondo, e nenhuma das duas falharia na compilação:** a união tem
**três** membros, não dois (um palpite de dois membros tem o mesmo tamanho e alinhamento, então
seria compatível por acidente), e `get_num_class` parece que deveria devolver a resposta.

**Resultado, contra o `predict` do próprio XGBoost sobre 256 linhas:**

| motor | pior diferença de probabilidade | classe escolhida |
|---|---:|---|
| avaliador compacto (tabelas) | 1,19e-07 | bate em todas |
| C99 do TL2cgen | 4,47e-08 | bate em todas |

Não é paridade bit a bit, e nem deveria ser: a ordem das somas de margem dentro do XGBoost não é a
mesma e os três somam em `float`. O que tem de bater exatamente é a **decisão**, e bate.

### E com o modelo REAL

O `run_all.sh` prefere `build/cfg` (modelo adotado, convertido por `tools/build_sensor_artefacts.sh`)
e só cai para o modelo de prova quando aquele não existe. Com o real, **as linhas de prova vêm do
próprio dataset** — 2048 linhas amostradas com passo fixo sobre os 14,2 M vetores e normalizadas com
os limites do treino. Isso importa: as regiões de decisão que o teste visita passam a ser as que o
sensor visita.

| motor | pior diferença | classe |
|---|---:|---|
| avaliador compacto | 2,38e-07 | bate em todas as 2048 |
| C99 do TL2cgen | 2,09e-07 | bate em todas as 2048 |

🔴 **E foi este teste que pegou o defeito do `base_score`.** O modelo adotado tem `base_score` em
**vetor por classe**, que não cancela no softmax; lido com um xgboost mais antigo que o que treinou
o modelo, ele voltava como `0,5` escalar, **sem erro nenhum**. Ver
[`docs/ARTEFATOS.md`](../docs/ARTEFATOS.md).

## O teste de paridade do parser

```bash
python tests/gen_parser_vectors.py \
    --repro ../someip-ids-multiclass-contentext --out build/parity
```

O gerador monta 14 quadros Ethernet crus à mão e deixa o `someip.parse` **real** dizer o que
cada um vira, inclusive os que ele descarta. Montar à mão é de propósito: aqui eu escolho os
casos, inclusive os que o dataset pode não conter (VLAN, IPv6, opções de IP, padding).

**Quatro comportamentos da referência parecem defeito e são reproduzidos assim mesmo**, porque o
modelo foi treinado com o que aquele código produz:

| comportamento | por que "consertar" quebraria |
|---|---|
| payload de transporte **menor que os 16 bytes** do cabeçalho SOME/IP não é falha | a referência devolve pacote com payloads vazios, e o extrator produz vetor a partir dele |
| quadro com **VLAN é descartado** | o EtherType é comparado com `0x0800` exato; tratar a etiqueta faria este parser ver pacotes que a referência nunca viu |
| o payload vai até o **fim do quadro**, não até `ip_len` | o padding do Ethernet entra no payload, é pontuado pelo modelo de bytes e medido pelas features de tamanho |
| IPv6 e fragmentação não existem | — |

⚠️ **O caso mais instrutivo do conjunto saiu do gerador, não da minha leitura do código.** Uma
carga UDP de **2 bytes**, num quadro completado até 60 pelo padding, vira um `pl_l4` de 18 bytes —
e a referência lê um cabeçalho SOME/IP **dentro do padding**, com `service_id = 0x0102`, e devolve
2 bytes de "corpo". O parser de referência **inventa uma mensagem a partir de bytes de
enchimento**. Cortar o payload em `ip_len`, que é o que se faria sem pensar, consertaria o
comportamento e quebraria a paridade.

**Uma divergência deliberada:** a referência não verifica limites. Em Python o fatiamento é
seguro, mas `raw[14]`, `raw[23]` e `raw[l4+12]` são indexações, e num quadro curto demais elas
**estouram**. Em C++ isso seria leitura fora dos limites, não exceção, então o quadro é rejeitado
com `MalformedPacket`. A divergência só pode aparecer onde o Python já quebrava.

**Resultado: 14 casos, 0 divergências.**

## O teste de paridade do extrator

É o teste que sustenta a segunda contribuição: se as features divergirem do Python,
o modelo treinado lá não vale aqui, e isso acontece **em silêncio**.

```bash
python tests/gen_parity_vectors.py \
    --repro ../someip-ids-multiclass-contentext --out build/parity

g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -fsanitize=address,undefined \
    -o /tmp/tp tests/test_parity_streamer.cpp \
    src/ids_sensor/ByteModel.cpp src/ids_sensor/FeatureStreamer.cpp \
    src/ids_sensor/IdsSensorErrorDomain.cpp

/tmp/tp build/parity/parity_bytemodels.bin build/parity/parity_vectors.txt
```

**A referência não é uma segunda implementação minha.** O gerador importa o
`FeatureStreamer` real de `src/extract_ext.py` do repositório de reprodução, que é
o código com o qual o modelo foi treinado e avaliado.

A comparação é **exata**, bit a bit no `float` de saída, sem tolerância. Isso é
possível porque a tabela de log-probabilidade é `double` nos dois lados, então as
somas acontecem sobre os mesmos float64 na mesma ordem. Uma tolerância esconderia
justamente o desvio sistemático pequeno que o teste existe para pegar.

**Resultado: 23 pacotes × 16 features = 368 comparações, 0 divergências.**

### Ele é sensível? Sim, e isso foi verificado

Um teste que passa de primeira merece desconfiança. Quebrando uma coisa por vez:

| quebra deliberada | divergências |
|---|---:|
| janela de repetição rejeitando payload vazio | 2 |
| janela de diversidade aceitando payload vazio | 14 |
| diversidade com janela de 1 valendo 1 em vez de 0 | 2 |
| entropia dividida por L em vez de k | 43 |
| Hamming sem a diferença de comprimento | 10 |
| primeiro pacote do fluxo com intervalo real em vez de 0 | 5 |

⚠️ **A primeira linha só passou a ser pega depois de corrigir o gerador.** A versão
inicial dele tinha um único pacote com `pl_someip` vazio, o de Service Discovery —
que usa as portas de SD e portanto cai em **outro fluxo**. Como a janela de
repetição é por fluxo, a assimetria nunca era exercitada, e rejeitar o vazio
passava no teste. Hoje há um pacote de corpo vazio **no mesmo fluxo** dos não-vazios.

### O teto de fluxos, que o Python não tem

O extrator tem `maxFlows`/`maxSources`; o Python cresce sem limite. No instante em
que o despejo dispara, os dois deixam de calcular a mesma coisa. Por isso o teste
**exige que `FlowEvictions()` e `SourceEvictions()` sejam zero** — sem isso, uma
divergência causada por despejo pareceria erro de aritmética.

⚠️ **E foi exatamente isso que a rodada sobre o dataset achou.** O `maxFlows` valia 4096; o
`mitm_single_attacker` tem **43 289** fluxos distintos e o `dos_noti_flood` tem **37 349**, porque
os dois randomizam portas. O valor hoje é 65 536, medido. Ver
[`platform/parity-dataset`](../platform/parity-dataset/README.md).

## A paridade sobre o dataset inteiro

```bash
PCAP_DIR=~/dataset/data/pcap bash platform/parity-dataset/run.sh
```

Os 23 pacotes sintéticos cobrem o que eu soube imaginar. Esta rodada cobre o que eu não soube:
**14 375 392 quadros, 14 233 354 vetores, 0 divergências**, pela cadeia de produção inteira
(`PcapSource → SomeipParser → FeatureStreamer`). É o número publicável, e está documentado com os
achados em [`platform/parity-dataset/README.md`](../platform/parity-dataset/README.md).

Ela não entra no `run_all.sh`: lê 1,4 GB, escreve 966 MB e o lado Python leva 21 minutos.

## Pendências frente à §10 do padrão

- [ ] migrar para **GoogleTest** (`TEST_F` com fixture por unidade), que é o que a
      §10 pede; hoje são asserções próprias, para não trazer dependência antes de
      existir build
- [ ] registrar com `add_test` e rodar por **CTest**
- [ ] cobertura por **gcovr**
- [ ] fixture que confere o heap restaurado no `TearDown`
- [ ] `test_<unidade>.cpp` para cada estágio, quando houver implementação
