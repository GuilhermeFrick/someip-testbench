# Testes

Rodam no **host**, nunca no alvo, como a §10 do padrão de código exige.

| Arquivo | O que verifica |
|---|---|
| `test_ara_core.cpp` | o subconjunto [`src/ara`](../src/ara/) com os tipos reais do pipeline |

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

## Pendências frente à §10 do padrão

Nenhuma destas vale ainda, porque o componente é só declaração: não há `.cpp` para
cobrir.

- [ ] migrar para **GoogleTest** (`TEST_F` com fixture por unidade), que é o que a
      §10 pede; hoje são asserções próprias, para não trazer dependência antes de
      existir build
- [ ] registrar com `add_test` e rodar por **CTest**
- [ ] cobertura por **gcovr**
- [ ] fixture que confere o heap restaurado no `TearDown`
- [ ] `test_<unidade>.cpp` para cada estágio, quando houver implementação
