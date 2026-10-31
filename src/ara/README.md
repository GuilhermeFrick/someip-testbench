# `ara` — subconjunto de bancada da AUTOSAR Adaptive Platform

Este diretório **não é** uma implementação da Adaptive Platform. É o pedaço mínimo
da especificação que a bancada usa de fato, escrito aqui porque **não existe
`ara::core` aberto de prateleira**: o vsomeip cobre só a camada de comunicação.

É a mesma decisão que a arquitetura já tomou para a `libecu` ("fachada estilo
`ara::com` sobre vsomeip", decisão 4 do [ARQUITETURA.md](../../docs/ARQUITETURA.md)),
estendida aos tipos de núcleo.

**O critério que governa tudo aqui:** manter os nomes e as assinaturas da
especificação. Portar para um stack AP de verdade deve ser **apagar este
diretório** e apontar os `#include` para o do fornecedor, sem tocar em
`src/ids_sensor`.

## O que existe

| Arquivo | Para que serve na bancada |
|---|---|
| [`core/Span.hpp`](core/Span.hpp) | vista sobre bytes do frame, sem copiar |
| [`core/Optional.hpp`](core/Optional.hpp) | valor que pode faltar **sem ser falha**: fim de fluxo, pacote que não gera vetor |
| [`core/ErrorDomain.hpp`](core/ErrorDomain.hpp) | identidade de um conjunto de erros, para o código 1 de um componente não se confundir com o 1 de outro |
| [`core/ErrorCode.hpp`](core/ErrorCode.hpp) | erro **tipado**, construído de um enumerador do projeto via ADL |
| [`core/Result.hpp`](core/Result.hpp) | valor **ou** erro: o canal de falha do AP |
| [`core/InstanceSpecifier.hpp`](core/InstanceSpecifier.hpp) | caminho de meta-modelo que nomeia uma instância configurada |
| [`idsm/EventReporter.hpp`](idsm/EventReporter.hpp) | reporte de **um** tipo de evento de segurança ao IdsM |

## Por que o `Result` importa aqui

Antes desta mudança, `Normalizer::Load` e `ByteModels::Load` **não tinham canal de
falha nenhum**. Retornavam por valor, e o que acontecia com um artefato corrompido
era indefinido. O `Result` fecha isso, e o domínio de erro do componente está em
[`../ids_sensor/IdsSensorErrorDomain.hpp`](../ids_sensor/IdsSensorErrorDomain.hpp).

Note a diferença de papel, que é fácil de errar:

- **`Result`** quando a operação pode **falhar** e o chamador precisa saber por quê;
- **`Optional`** quando o valor pode legitimamente **faltar** e isso é normal.

Fim de fluxo num PCAP é `Optional`. Arquivo corrompido é `Result`.

## As quatro regras do AUTOSAR que mudaram o código

| Regra | O que exige | O que mudou |
|---|---|---|
| **A8-4-8** | parâmetros de saída são proibidos | seis assinaturas passaram a **retornar** valor: `Parse`, `Next`, `Step`, `Predict`, `Apply`, e o par log-verossimilhança/entropia virou um `PayloadScores` |
| **A8-4-4** | múltiplos retornos vêm como struct | `Both(b, &loglik, &ce)` virou `Score(b) -> PayloadScores` |
| **A16-0-1** | pré-processador só para include guard e `#include` | a macro de atributo que eu havia criado saiu |
| **A0-1-2** | o retorno de função não-void deve ser usado | é a regra que o `[[nodiscard]]` tentava expressar; aqui ela é verificada por análise estática, e retornar valor em vez de escrever em referência é o que a torna aplicável |

## Onde isso conflita com o padrão de código C++ de referência

Dois pontos, e em ambos o AUTOSAR venceu porque é o alvo da plataforma:

1. A §7 do padrão **recomenda** sobrecarregar em parâmetro de saída
   (`bool Get(Capacitive_st &cap)`). A A8-4-8 **proíbe**.
2. A §8.5 do padrão quer `outcome<T>` com mensagem de **string**, e testes que
   afirmam a mensagem literalmente. O AP quer `ErrorCode` **tipado** com domínio.
   Aqui a mensagem é diagnóstico, não interface: o teste compara enumerador, que o
   compilador confere.

O resto do padrão continua valendo integralmente: formatação, Doxygen com `@retval`
por valor, nomes, `m_` nos membros, construção travada por `Create()` privada.

## Desvios da especificação

Estão anotados no `@details` de cada arquivo. Os principais:

- sem `StringView`: onde a spec a usa, aqui é `const char *` ou `const std::string &`;
- `Span` só com extensão dinâmica;
- `ErrorCode::ThrowAsException()` não existe, porque exceções não são o mecanismo
  de sinalização;
- `Result` sem os helpers monádicos (`AndThen`, `OrElse`, `Map`) e sem a
  especialização `Result<void>`;
- `ContextDataType` do `idsm` é uma vista, não um `Vector`, para o reporte não alocar.

## Verificação

`tests/test_ara_core.cpp` exercita os templates com os tipos reais do pipeline,
incluindo um tipo **sem construtor padrão** (`ByteModels`) e um **move-only**
(`unique_ptr`), e conta destrutores para provar que o `aligned_storage` não vaza nem
destrói duas vezes. Roda sob AddressSanitizer e UndefinedBehaviorSanitizer:

```bash
g++ -std=c++14 -I. -Wall -Wextra -Wpedantic -fsanitize=address,undefined \
    -o t tests/test_ara_core.cpp && ./t
```

Passa também com `-std=c++17`.
