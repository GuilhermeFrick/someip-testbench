# Experimento: libxgboost x TL2cgen

## A pergunta

Na primeira contribuição medimos **~500 µs por pacote** na inferência de **uma** amostra, contra
**2,6 µs** amortizados em lote. Como a travessia de 300 árvores de profundidade 8 custa cerca de
**1 µs** de CPU real, os 500 µs são **custo fixo por chamada**, não cálculo.

A hipótese corrente era "é a fronteira Python". Mas há um contra-indício: **`inplace_predict` e
`predict_proba` deram os dois ~500 µs**, e o `inplace_predict` já é um wrapper fino sobre a C API,
sem construir DMatrix. Isso sugere que **parte do custo esteja dentro da própria `libxgboost`**.

Este experimento decide entre as duas explicações, e ao mesmo tempo compara as duas opções de
implementação para o `ids_sensor`:

| Hipótese | Se verdadeira, o resultado será |
|---|---|
| O custo era a fronteira Python | `libxgboost` chamada do C dá **poucos µs** |
| O custo é intrínseco ao caminho de 1 linha da `libxgboost` | C também dá **centenas de µs**, e o **TL2cgen ganha por ordens de grandeza** |

## O método

Os dois motores são medidos **no mesmo programa C**, com o **mesmo relógio** (`CLOCK_MONOTONIC`),
o **mesmo laço** e as **mesmas linhas de entrada**. Isso elimina diferença de máquina, de
compilador e de instrumentação, que invalidaria a comparação.

A **paridade numérica** é verificada contra o `inplace_predict` do Python (o oráculo): para cada
linha, o maior desvio absoluto nas probabilidades e se a classe predita bate.

Nota sobre as entradas: as 16 features entram normalizadas em [0,1] no pipeline, então usamos
valores uniformes com semente fixa. Para esta medição o que importa é que Python e C recebam
**exatamente as mesmas linhas**, não que elas venham de tráfego real.

## Como rodar

Pré-requisitos: `gcc`, e no ambiente Python `xgboost`, `treelite` e `tl2cgen`.

```bash
pip install treelite tl2cgen

# 1. exporta o C do TL2cgen, gera entradas + oraculo, mede o Python
python prepare.py --model /caminho/para/results/model_16.json --out build

# 2. compila o head-to-head
bash build.sh build

# 3. roda
./build/bench build /caminho/para/results/model_16.json 2000
```

O `prepare.py` **imprime o `header.h` gerado pelo TL2cgen**. Se a assinatura de
`predict_multiclass` for diferente da assumida no `bench.c`, é só ajustar a chamada ali.

## Como ler o resultado

```
=== libxgboost (C API, XGBoosterPredictFromDense, nthread=1) ===
  latencia        :    ???.?? us/pred
  paridade        : desvio max ?, classes divergentes 0/512

=== TL2cgen (C99 gerado) ===
  latencia        :    ???.?? us/pred
  paridade        : desvio max ?, classes divergentes ?/512
```

Decisão conforme o número da `libxgboost` em C:

| Resultado | Leitura | Consequência para a bancada |
|---|---|---|
| **poucos µs** | o custo era Python | Seguir com `libxgboost`, como planejado |
| **centenas de µs** | custo intrínseco da lib | **TL2cgen volta ao caminho crítico**, ou adota-se micro-batching |

Atenção à paridade do TL2cgen: se as probabilidades divergirem, provavelmente ele devolve
**margens brutas** em vez de aplicar o softmax do `multi:softprob`. Nesse caso, aplicar o softmax
no lado C e repetir a conferência — não é um defeito, é uma convenção de saída.

## Por que isso importa agora

A Fase 3 do [plano](../../docs/PLANO.md) assume `libxgboost` no `ids_sensor`. Essa escolha é a mais
simples e a de menor risco numérico, mas **repousa sobre uma premissa não verificada**. Este
experimento é barato e evita construir o nó inteiro sobre ela.
