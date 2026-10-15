# Resultados: libxgboost x TL2cgen

**Máquina:** AWS EC2 `c7a.2xlarge`, AMD **EPYC 9R14** (Zen 4), 8 vCPU sem SMT, Ubuntu 24.04,
gcc 13.3, xgboost 3.3.0, treelite 4.7.2, tl2cgen 1.0.0. Medição fixada em **1 núcleo**
(`taskset -c 0`, `nthread=1`), 512 linhas distintas, 2000 repetições.

## Números

| Caminho | µs por predição | Razão vs. piso |
|---|---:|---:|
| Python, 1 linha por chamada (`inplace_predict`) | **716,6** | 33x |
| `libxgboost` C API, 1 linha por chamada | **534,8** | 25x |
| **TL2cgen** (C99 gerado), 1 linha por chamada | **109,4** | 5x |
| `libxgboost` em lote (512 linhas, 1 núcleo) | **21,5** | 1x (piso) |

**Paridade numérica:** ambos corretos.

| Motor | Desvio máximo | Classes divergentes |
|---|---|---|
| `libxgboost` C API | 0,000e+00 (exato) | 0 / 512 |
| TL2cgen | 1,609e-06 (arredondamento float) | 0 / 512 |

O TL2cgen trata o `multi:softprob` de 5 classes corretamente: o `predict(..., pred_margin=0, ...)`
já aplica o softmax internamente. Aquele risco está descartado.

## O que isso derruba

**A hipótese de que os ~500 µs eram a fronteira Python estava errada.** Chamando a C API direto do
C, o custo caiu de 716,6 para 534,8 µs, ou seja, **o Python respondia por apenas ~180 µs (25%)**.
Os **~535 µs restantes são intrínsecos ao caminho de predição de uma linha da `libxgboost`**.

Isso **reverte a decisão** de usar a `libxgboost` no `ids_sensor` em regime por pacote: ela é a
**pior** das três opções nesse regime.

## O que o número do TL2cgen revela

O TL2cgen ganha por **4,9x**, mas ainda fica **5x acima do piso** de 21,5 µs, que é o custo real de
computação do modelo em 1 núcleo. A explicação mais provável é **pressão de cache de instruções**:
o C gerado tem **323.231 linhas** num único arquivo, e o código compilado das 1500 árvores
(300 rodadas × 5 classes) não cabe em cache. Cada predição percorre megabytes de instruções.

Ou seja, a geração de código **não escala** para ensembles grandes: o problema deixa de ser
computação e vira tráfego de instruções.

Isso sugere que a abordagem de **tabelas `const` mais um avaliador de laço limitado** (o caminho
pensado para ECU) deve ser **mais rápida que o TL2cgen**, porque troca megabytes de código por
poucos KB de dados, que cabem em cache. É uma hipótese a testar, não um resultado.

## Custo prático do TL2cgen

- **Compilação: 3 min 44 s** para o arquivo gerado, num único núcleo (o TL2cgen avisa que desativou
  a compilação paralela). Vira custo fixo do pipeline de build.
- A `libxgboost` ocupa **190 MB** e seu `SONAME` é `libxgboost.so.3`, mas o wheel só instala
  `libxgboost.so`, então é preciso criar o symlink para linkar em C.

## Leitura para a decisão

1. **Micro-batching continua sendo a maior alavanca.** O piso de 21,5 µs por predição só é
   alcançado em lote, e nenhum motor em regime unitário chega perto dele.
2. **Se o regime unitário for obrigatório, é TL2cgen, não `libxgboost`** (109 vs 535 µs).
3. **Vale testar o avaliador compacto**, que pode superar os dois ao evitar a explosão de código.

## Como reproduzir

Ver [README.md](README.md). Resumo:

```bash
pip install treelite tl2cgen
python prepare.py --model .../results/model_16.json --out build
bash build.sh build
taskset -c 0 ./build/bench build .../results/model_16.json 2000
```
