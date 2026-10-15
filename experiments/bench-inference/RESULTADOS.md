# Resultados: libxgboost x TL2cgen x avaliador compacto

**Máquina:** AWS EC2 `c7a.2xlarge`, AMD **EPYC 9R14** (Zen 4), 8 vCPU sem SMT, Ubuntu 24.04,
gcc 13.3, xgboost 3.3.0, treelite 4.7.2, tl2cgen 1.0.0. Medição fixada em **1 núcleo**
(`taskset -c 0`, `nthread=1`), 512 linhas distintas, 2000 repetições.

**Modelo:** `multi:softprob`, 5 classes, 300 rodadas → **1500 árvores**, **162.334 nós**.

## Números

| Caminho | µs por predição | vs. piso | vs. `libxgboost` |
|---|---:|---:|---:|
| Python, 1 linha por chamada (`inplace_predict`) | 716,6 | 33x | 0,7x |
| `libxgboost` C API, 1 linha por chamada | 502,4 | 23x | 1x |
| TL2cgen (C99 gerado), 1 linha por chamada | 111,3 | 5,2x | **4,5x** |
| **Avaliador compacto** (tabelas `const`), 1 linha | **48,1** | **2,2x** | **10,4x** |
| `libxgboost` em lote (512 linhas, 1 núcleo) | 21,5 | 1x (piso) | — |

Variação entre execuções da ordem de ±6% (a `libxgboost` deu 534,8 e 502,4 em duas rodadas).

**Paridade numérica contra o `inplace_predict` do Python:**

| Motor | Desvio máximo | Classes divergentes |
|---|---|---|
| `libxgboost` C API | 0,000e+00 (exato) | 0 / 512 |
| TL2cgen | 1,609e-06 | 0 / 512 |
| **Avaliador compacto** | **0,000e+00 (exato)** | 0 / 512 |

## Conclusão 1: a hipótese do Python estava errada

Chamando a C API direto do C, o custo caiu de 716,6 para ~502 µs: **o Python respondia por cerca de
25%**. Os **~500 µs restantes são intrínsecos ao caminho de predição de uma linha da `libxgboost`**.

Isso **reverteu a decisão** de usar a `libxgboost` no `ids_sensor` em regime por pacote: ela é a
**pior** das três opções nesse regime.

## Conclusão 2: o problema do TL2cgen é tamanho de código

O TL2cgen ganha da `libxgboost` por 4,5x, mas perde do avaliador compacto por 2,3x. A causa é
tamanho de **código**:

| Representação | Tamanho | Natureza |
|---|---:|---|
| C gerado pelo TL2cgen | 323.231 linhas → **objeto de 11,7 MB** | código |
| Tabelas do avaliador compacto | **2,60 MB** (162.334 nós × 16 B) | dado |

O código das 1500 árvores não cabe em cache de instruções, então cada predição percorre megabytes
de instruções. **Geração de código não escala para ensembles grandes.** Trocar código por dado
resolve: a tabela é 4,5x menor e é percorrida como dado, não executada.

## Conclusão 3: o avaliador compacto domina em todos os eixos

São cerca de 30 linhas de C:

```c
typedef struct { float thr; int32_t feat; int32_t left; int32_t right; } node_t;  /* 16 B */

for (int t = 0; t < n_trees; ++t) {
    int32_t i = roots[t];
    for (int d = 0; d < DEPTH_GUARD; ++d) {          /* laco limitado */
        if (nodes[i].feat < 0) break;                /* folha */
        i = (x[nodes[i].feat] < nodes[i].thr) ? nodes[i].left : nodes[i].right;
    }
    margin[t % n_class] += nodes[i].thr;
}
softmax(margin, n_class);
```

| Critério | `libxgboost` | TL2cgen | Avaliador compacto |
|---|---|---|---|
| Latência (1 linha) | 502 µs | 111 µs | **48 µs** |
| Paridade | exata | 1,6e-06 | **exata** |
| Dependências | 190 MB de `.so` | nenhuma | **nenhuma** |
| Tempo de build | — | **3 min 44 s** | **instantâneo** |
| Alocação dinâmica no caminho quente | sim | não | **não** |
| WCET analisável | não | difícil | **sim** (`n_trees × DEPTH_GUARD`) |

Ou seja, a opção mais rápida é também a mais simples, a mais portável e a única com WCET
analisável. Não há trade-off a fazer aqui.

## Armadilha encontrada (e que a paridade pegou)

O `base_score` deste modelo **não é escalar, é um vetor por classe**:
`[2,7467, -1,0028, -0,3698, -0,7514, -0,6228]`.

A suposição inicial era de que, sendo escalar, ele cancelaria no softmax (que é invariante a
deslocamento) e poderia ser ignorado. **Sendo vetor, ele não cancela** e precisa entrar como margem
inicial de cada classe. Se isso passasse batido, o detector erraria silenciosamente.

É o argumento a favor de tratar a conferência de paridade como **critério de aceite**, não como
verificação opcional.

## O que ainda sobra

O avaliador compacto está em 48,1 µs contra um piso de 21,5 µs em lote. Os 2,2x restantes devem vir
de falhas de cache de **dados** (a tabela tem 2,6 MB, acima do L2) e do laço por árvore. Caminhos
para fechar essa diferença, se for necessário:

- reordenar os nós por profundidade para melhorar localidade;
- encolher o nó (índice de feature em `int16`, filhos relativos) para caber mais em cache;
- **micro-batching**, que continua sendo a alavanca de maior efeito e menor esforço.

## Como reproduzir

Ver [README.md](README.md). Resumo:

```bash
pip install treelite tl2cgen
python prepare.py       --model .../results/model_16.json --out build
python export_tables.py --model .../results/model_16.json --out build
bash build.sh build
taskset -c 0 ./build/bench build .../results/model_16.json 2000
```
