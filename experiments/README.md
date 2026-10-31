# Experimentos da bancada

Dois experimentos, uma investigação. Eles se citam mutuamente e **não devem ser
lidos em separado**: juntos respondem uma pergunta só, *o que a bancada embarca*.

| Pasta | Pergunta | Resposta |
|---|---|---|
| [`model-sweep/`](model-sweep/) | qual tamanho de modelo | **25 rodadas, profundidade 8** (24.329 nós) |
| [`bench-inference/`](bench-inference/) | qual motor de inferência | **TL2cgen** (1,39 µs) |

A ordem em que foram feitos inverteu a conclusão do segundo. O `bench-inference`
mediu primeiro com o modelo antigo de 300 rodadas e concluiu que a `libxgboost`
era intrinsecamente lenta no regime de uma linha. O `model-sweep` mostrou depois
que aquele modelo era 23x maior que o necessário, e refazer o confronto com o
modelo certo **inverteu o ranking dos motores**: o TL2cgen saltou de 107,2 para
1,39 µs e passou à frente do avaliador compacto.

A lição está registrada nos dois documentos: o fator dominante era **tamanho de
modelo e cache**, não o motor. Otimizar dentro de um modelo fixo mediu a coisa
errada.

## Relação com o repositório de reprodução

A varredura de hiperparâmetros aparece **nos dois repositórios**, e isso é
deliberado. Não é duplicata esquecida.

| | aqui (`someip-testbench`) | [`someip-ids-multiclass-contentext`](https://github.com/GuilhermeFrick/someip-ids-multiclass-contentext) |
|---|---|---|
| pergunta | o que **embarcar** | o que **publicar** |
| script | `model-sweep/select_on_val.py` | `select_hyperparams.py` |
| o que mais registra | `n_nodes`, `tabela_mb`, `p50_us`, `p99_us` | F1 por classe na validação, contagem de árvores |
| ablação | `feature_ablation.py`, modelo `r50_d4`, 3 sementes | `ablate_features.py` e `loo.py`, modelo adotado |

Os dois chegam ao **mesmo número**: as 15 configurações dão macro-F1 de validação
idêntico até a última casa decimal nos dois repositórios, porque o treino é
determinístico (`tree_method=hist`, sem `subsample` nem `colsample`, então
`random_state` não tem o que sortear). Foi isso, aliás, que mostrou que a
repetição de semente deste experimento media nada: as 21 execuções do
`ablation.json` têm desvio **zero**.

**Quem é canônico para quê:** para o artigo, o repositório de reprodução. Para a
decisão de embarque, este, porque só aqui existem as colunas de latência e
footprint em C, que o `sweep_eval.c` mede e que são metade do critério.

## O que é só daqui

- `model-sweep/sweep_eval.c` — avaliador de árvores em C, mede latência por
  configuração sem linkar `libxgboost` nem TL2cgen, então compila em segundos.
- `bench-inference/bench.c` — os três motores no **mesmo programa**, mesmo
  relógio, mesmo laço, mesmas linhas de entrada. É o que torna a comparação
  válida.

## Papel residual do avaliador compacto

Com o modelo adotado ele é 2x mais lento que o TL2cgen, então **sai do caminho
quente**. Fica como **segundo oráculo de paridade**, ao lado da `libxgboost`,
porque tem paridade exata (0,0 contra 3,87e-07) e WCET limitado por construção.

Não é papel menor: foi a conferência cruzada entre oráculos que revelou que o
`base_score` deste modelo é um **vetor por classe**, e não um escalar. Como o
softmax é invariante a deslocamento, um `base_score` escalar cancelaria e poderia
ser ignorado; um vetor não cancela. O detector erraria em silêncio.
