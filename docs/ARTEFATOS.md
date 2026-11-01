# Os artefatos do treino, e por que eles conferem

O `ids_sensor` carrega **três** artefatos, e todos têm de vir da mesma rodada de treino: o modelo
de árvores, as tabelas do modelo de bytes e os limites de normalização. Misturar rodadas desloca a
fronteira de decisão **sem levantar erro nenhum** — é para isso que o
`IdsSensorErrc::TrainingRunMismatch` existe (e segue sem produtor: nenhuma verificação cruzada
está implementada).

```bash
bash tools/build_sensor_artefacts.sh <dir-com-os-artefatos-do-treino>
```

| do treino | vira | lido por |
|---|---|---|
| `model_16.json` | `tables.bin` + `tl2cgen_c/` | `CompactClassifier` e `Tl2cgenClassifier` |
| `bytemodels.npz` | `bytemodels.bin` | `ByteModels::Create` |
| `minmax.npy` | `minmax.bin` | `Normalizer::Create` |

## As datas não fechavam

Os artefatos foram recuperados da VM de treino em 01/11/2026, e as datas deles **não são da mesma
rodada**:

| artefato | data | o que é |
|---|---|---|
| `metrics_temporal.json` | 12/10 20:48 | run ANTIGO: 300 rodadas, macro-F1 0,9639 |
| `minmax.npy`, `bytemodels.npz` | 12/10 23:13 | de um run intermediário |
| `model_16.json` | **15/10 23:26** | o modelo ADOTADO: 25 rodadas, 125 árvores |

Isso é exatamente o que o `TrainingRunMismatch` existe para pegar. Mas dá para **verificar**, e não
apenas torcer, porque os limites e as tabelas de bytes dependem só dos PCAPs e do código de
extração, não dos hiperparâmetros do modelo. Os dois foram recalculados do zero e comparados:

| artefato | resultado da verificação |
|---|---|
| `minmax.npy` | **idêntico bit a bit** ao recálculo em float64 a partir dos 7 PCAPs (md5 igual) |
| `bytemodels.npz` | `sd` idêntico bit a bit; `someip` e `l4` dentro de **1,8e-15**, ~1 ULP de float64 |
| `model_16.json` | os dois motores em C++ reproduzem o `predict` do XGBoost em 2048 linhas reais, dentro de **2,4e-07**, com a classe escolhida batendo em todas |

**Por que um é exato e o outro não.** Mínimo e máximo são *seleções* de valores que já existem: não
há aritmética, logo não há ordem de acumulação e o resultado é determinístico até o último bit. As
tabelas de bytes são *somas e logaritmos*, e aí a ordem de acumulação importa — a diferença de
1,8e-15 é ruído de versão de `numpy`, não de código.

**Conclusão: apesar das datas, os três artefatos são mutuamente consistentes.** E agora isso é um
resultado verificado, não uma suposição.

## A versão do xgboost não é escolha livre

🔴 **O modelo foi salvo pelo xgboost 3.3.0. Lido com o 2.1.3, ele carrega sem erro nenhum e
devolve `base_score = 0,5`** em vez do vetor por classe que está no arquivo:

```
base_score no ARQUIVO : [2,7467, -1,0028, -0,3698, -0,7514, -0,6227]
base_score lido pelo 2.1.3 : [0,5, 0,5, 0,5, 0,5, 0,5]
```

Como aqui o `base_score` é um **vetor**, ele **não cancela no softmax**: as margens iniciais sairiam
erradas e as probabilidades junto, sem nada falhar. O `tables.bin` da primeira conversão tinha esse
defeito.

Vale o contraste: o **treelite** 4.4.1 também não lê o JSON do 3.3.0, mas ele **estoura**. O
xgboost antigo cala. Dos dois modos de falhar, o que custa caro é o segundo.

Duas defesas, as duas aplicadas:

1. a `deploy/Dockerfile.parity` fixa `xgboost==3.3.0`, `treelite==4.8.1` e `tl2cgen==1.0.0`, com o
   motivo escrito ali;
2. o `export_tables.py` passou a ler o `base_score` **do arquivo do modelo**, não do
   `save_config()`, e a dizer se ele é escalar ou vetor em vez de só imprimir a lista.

O que pegaria isso de qualquer jeito é o oráculo: a comparação dos dois motores contra o `predict`
do próprio XGBoost, em `tests/test_classifier.cpp`.

## O tamanho do C gerado depende muito do modelo

| modelo | linhas de C | objeto | tabela de nós |
|---|---:|---:|---:|
| 300 rodadas × profundidade 8 | 323 000 | 11,7 MB | — |
| **25 rodadas × profundidade 8 (adotado)** | **48 596** | **1,77 MB** | **0,39 MB** (24 329 nós) |

Foi com o primeiro que a pressão de cache de instruções apareceu no `bench-inference`. Com o
adotado o código gerado é **seis vezes menor**, e compila em 19 s.

## Limites de normalização, medidos

Ver [`platform/parity-dataset`](../platform/parity-dataset/README.md) para o achado do
`ip_time_interval` com mínimo negativo e para o que isso diz sobre min-max calculado no dataset
inteiro.

## O que ainda falta

- **`TrainingRunMismatch` segue sem produtor.** Fechá-lo exige uma marca de rodada nos três
  artefatos — hoje a consistência foi verificada à mão, uma vez, e não é verificável em runtime.
- O `metrics_temporal.json` recuperado é do run de **300 rodadas**, não do adotado. As métricas
  publicadas do modelo adotado estão em `results-publicados/` no repositório de reprodução.
