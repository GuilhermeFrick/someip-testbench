# Testes

Rodam no **host**, nunca no alvo, como a §10 do padrão de código exige.

```bash
bash tests/run_all.sh          # roda tudo, com os sanitizers ligados
```

| Arquivo | Tipo | O que verifica |
|---|---|---|
| `test_ara_core.cpp` | unitário | o subconjunto [`src/ara`](../src/ara/), incluindo `Result<void>` |
| `test_bytemodel.cpp` | unitário | `HammingDistance`, a pontuação **posicional** e os erros de carga do artefato |
| `test_feature_streamer.cpp` | unitário | validação, `Reset`, isolamento das janelas e o **despejo** |
| `test_libecu.cpp` | unitário | os **caminhos de erro** da fachada, que o handshake não toca |
| `test_comm_matrix.cpp` + `check_comm_matrix.py` | consistência | o cabeçalho gerado contra o YAML da matriz |
| `test_parity_streamer.cpp` + `gen_parity_vectors.py` | oráculo | **as 16 features contra o extrator do Python** |
| [`platform/node-handshake`](../platform/node-handshake/) | integração | dois nós trocando SOME/IP em containers |

**104 asserções**, todas sob AddressSanitizer e UndefinedBehaviorSanitizer.

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

## Pendências frente à §10 do padrão

- [ ] migrar para **GoogleTest** (`TEST_F` com fixture por unidade), que é o que a
      §10 pede; hoje são asserções próprias, para não trazer dependência antes de
      existir build
- [ ] registrar com `add_test` e rodar por **CTest**
- [ ] cobertura por **gcovr**
- [ ] fixture que confere o heap restaurado no `TearDown`
- [ ] `test_<unidade>.cpp` para cada estágio, quando houver implementação
