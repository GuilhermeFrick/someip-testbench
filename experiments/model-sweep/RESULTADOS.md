# Resultados: encolhimento do modelo

**Máquina:** AWS EC2 `c7a.2xlarge`, AMD EPYC 9R14 (Zen 4), 1 núcleo (`taskset -c 0`), xgboost 3.3.0.
**Dataset:** completo, 14.233.354 pacotes; split temporal (treino 9.963.346 / teste 4.270.008).
**Latência:** avaliador compacto, 3000 chamadas por configuração, 1 linha por chamada.

Metodologia inspirada em Dal Zotto & Moraes (SBCCI 2025), que encolhem o modelo XGBoost
iterativamente aceitando perda limitada da métrica, para tornar a inferência barata o bastante
para embarcar.

## O achado

O modelo do artigo (300 rodadas, profundidade 8) **não está na fronteira qualidade × custo**.
Existe configuração **melhor em métrica e 17x mais barata**.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figs/fig-fronteira-dark.png">
  <img src="figs/fig-fronteira-light.png" alt="Fronteira macro-F1 contra latencia: o modelo do artigo fica no extremo caro da curva, enquanto r50_d4 atinge macro-F1 maior a 2,84 us">
</picture>

| | modelo do artigo | melhor da varredura | |
|---|---:|---:|---|
| configuração | 300 rodadas, prof. 8 | **50 rodadas, prof. 4** | |
| macro-F1 | 0,9640 | **0,9695** | melhor |
| nós | 162.334 | **7.136** | **23x menos** |
| tabela | 2,60 MB | **0,11 MB** | 23x menor |
| latência (p50) | 47,1 µs | **2,83 µs** | **16,7x mais rápido** |
| treino | 268,7 s | **64,7 s** | 4x mais rápido |

## Por classe: a classe difícil foi a que mais ganhou

A preocupação era que o macro-F1 se mantivesse enquanto a classe difícil (MITM-multi) degradasse.
**Aconteceu o oposto.**

| configuração | macro | normal | dos | fuzzy | mitm_single | **mitm_multi** |
|---|---:|---:|---:|---:|---:|---:|
| r300_d8 (artigo) | 0,9640 | 0,9935 | 0,9965 | 0,9988 | 0,9930 | **0,8380** |
| **r50_d4** | **0,9695** | 0,9946 | 0,9982 | 0,9968 | 0,9952 | **0,8630** |
| r25_d8 | 0,9689 | 0,9945 | 0,9981 | 0,9991 | 0,9947 | 0,8580 |
| r25_d4 | 0,9643 | 0,9945 | 0,9970 | 0,9899 | 0,9812 | 0,8591 |

A MITM-multi sobe de **0,8380 para 0,8630**. O modelo grande estava **sobreajustando**, e a classe
mais difícil era a que mais sofria com isso. Encolher não foi um sacrifício: foi correção.

Note também que **mais rodadas pioram** em todas as profundidades estáveis (prof. 4: 0,9695 em 50
rodadas contra 0,9676 em 300). É o comportamento esperado de sobreajuste sob split temporal, em que
o conjunto de teste é um período posterior.

## O que isso faz com o trabalho de otimização anterior

| Caminho | µs/pred |
|---|---:|
| Python, modelo do artigo | 716,6 |
| `libxgboost` C API, modelo do artigo | 533,2 |
| TL2cgen, modelo do artigo | 107,2 |
| avaliador compacto, modelo do artigo | 48,0 |
| **avaliador compacto + r50_d4** | **2,83** |

O "piso" de 21,5 µs que tínhamos medido em lote **não era um piso**: era propriedade de um modelo
grande demais. Com o modelo certo, uma predição unitária custa menos que um oitavo daquilo.

**A maior alavanca não era o motor de inferência, era o tamanho do modelo.** Toda a comparação entre
`libxgboost`, TL2cgen e avaliador compacto foi feita otimizando dentro de um modelo fixo, que por
sua vez estava 23x maior que o necessário.

## Anomalia não explicada: profundidade 6

| rodadas | 25 | 50 | 100 | 200 | 300 |
|---|---:|---:|---:|---:|---:|
| macro-F1 | 0,9658 | 0,8761 | **0,5744** | **0,5744** | **0,5744** |

O colapso é **determinístico e idêntico** em três configurações distintas, até a quarta casa. Não é
ruído: o modelo converge para o mesmo estado degenerado e rodadas extras não mudam nada. Por classe,
em `r100_d6`, o `dos` cai para 0,2782 e o `mitm_single` para 0,3761, enquanto `normal` fica em
0,9112, ou seja, a falha é concentrada em classes específicas, não uniforme.

Profundidades 4 e 8 são estáveis, o que torna o caso ainda mais estranho. **Não tenho explicação**,
e não investiguei a fundo porque nenhuma configuração candidata está nessa faixa. Fica registrado
como anomalia a investigar, não como conclusão.

## Consequência para o artigo

O artigo reporta 0,9640 com 300 rodadas e profundidade 8. Existe um modelo **melhor e muito mais
barato**. Trocar implica refazer matrizes de confusão, curvas ROC/PR, custo por categoria e as
tabelas de latência. É decisão em aberto, não aplicada.

## Como reproduzir

```bash
python sweep.py --repo <repo-do-ids> --out sweep     # extrai 1x, treina as 15 configuracoes
gcc -O2 -std=c99 -o sweep_eval sweep_eval.c -lm
taskset -c 0 ./sweep_eval sweep/<cfg>/tables.bin <rows.bin> 16 3000
python plot_sweep.py
```
