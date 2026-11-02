# Falso positivo do detector congelado, no barramento reproduzido

**Data:** 2026-11-01. **Máquina:** `runner`, imagem `someip-base:3.6.4`.
**Motor:** avaliador compacto (tabelas de nós), conferido contra o `predict` do XGBoost em
2048 linhas reais do dataset com pior diferença **2,38e-07**. O C99 do TL2cgen, ligado ao lado, deu
resultado **idêntico** (mesmos 9461 eventos, mesma repartição por classe) — o que também responde ao
aviso de versão do Treelite, que não corrompeu o código gerado.

## O que está sendo medido, e por que não precisa de rótulo

As capturas da bancada são benignas **por construção**: todos os nós são nossos, publicando na
cadência recuperada, e nenhum nó atacante é subido. Logo **todo evento levantado é falso positivo**,
e a razão é a taxa. Não há arquivo de ground truth a consultar. É a única coisa que um barramento
reproduzido dá e um dataset não.

Calibração e avaliação em **rodadas diferentes** do mesmo barramento. Calibrar e medir na mesma
captura daria número otimista por construção.

## O número de referência

O mesmo binário, sobre o benigno do próprio dataset:

| | |
|---|---:|
| pacotes aceitos | 2 193 802 |
| eventos | 597 |
| **falso positivo** | **0,0272%** |
| confiança mediana | 0,665 |

Concentrado em `172.18.0.4` (116) e `172.18.0.10` (481), serviços `0x1001` e `0x1003`.
**Guarde essa estrutura:** ela reaparece na bancada.

## O resultado

Avaliação na rodada **B** (payload `counter`) e na rodada **D** (payload `random`), ambas de 60 s,
~108 mil pacotes aceitos cada. Calibração nas rodadas **A** e **C**, respectivamente.

| conteúdo do payload | artefatos | falso positivo | × a referência | conf. mediana |
|---|---|---:|---:|---:|
| — (dataset) | do treino | **0,0272%** | 1× | 0,665 |
| `counter` | do treino | 9,42% | 346× | 0,934 |
| `counter` | limites **recalibrados** | **4,11%** | 151× | 0,843 |
| `counter` | limites **e** bytes recalibrados | 25,29% | 930× | 0,503 |
| `random` | do treino | 44,50% | 1636× | 0,900 |
| `random` | limites recalibrados | 49,62% | 1824× | 0,995 |
| `random` | limites e bytes recalibrados | 49,62% | 1824× | 0,738 |

### Três leituras, em ordem de importância

**1. Recalibrar os LIMITES ajuda, e ajuda pouco.** 9,42% → 4,11%, uma queda de 56%. É a camada mais
barata (min/max por coluna, sem rótulo) e funciona na direção prometida. Mas 4,11% continua **151×**
a referência: num barramento de 108 mil pacotes são 4 449 alarmes em 60 segundos. Não é operável.

**2. Recalibrar os MODELOS DE BYTES piora — e a causa é identificável.** 4,11% → 25,29%, quase todo
em `fuzzy` (23 031 de 27 380). O modelo de bytes ajustado no payload `counter` fica **degenerado**:
com os bytes quase sempre os mesmos, as tabelas vão de `min=-10,91` a `max=-0,0048` (quase
determinístico), e a log-verossimilhança e a entropia dos pacotes saem da faixa em que o
classificador foi treinado. Com payload `random` o mesmo modelo fica **chato** (`min=-6,42`,
`max=-3,52`, perto de `log(1/256) = -5,55`) e o resultado é igualmente ruim pelo motivo oposto.
Isto não é defeito da calibração: é o conteúdo do payload da bancada não ter a natureza estatística
de sinal nenhum.

**3. O conteúdo do payload domina tudo o mais.** `random` é **cinco vezes** pior que `counter` com
os mesmos artefatos (44,50% contra 9,42%). Byte aleatório é alta entropia, que é literalmente a
assinatura de `fuzzy`. O degrau 1 da escada do `DISCUSSAO.md` está medido, e o que ele diz é que
**nenhuma das duas políticas sintéticas serve**: a bancada precisa de conteúdo com a estatística de
sinal real, que é para o que a ancoragem no CARLA existe.

### Uma consistência que vale notar

No dataset, 481 dos 597 falsos positivos vêm de `172.18.0.10`, o provedor de `0x1001`. Na bancada,
9 787 dos 10 195 vêm de `172.28.0.10` — **o mesmo papel**, o provedor de `0x1001`, que é também o de
maior cadência (um evento a cada 5 ms). O nó mais difícil para o detector é o mesmo nos dois
barramentos; a bancada só tem muito mais dele. Isso é evidência de que a reprodução acertou a
estrutura, e que o que não transfere é o conteúdo.

## O que isto significa para a contribuição

A frase honesta **não** é "o detector funciona em qualquer barramento". É:

> As features e o classificador transferem a ESTRUTURA — o nó mais difícil é o mesmo papel nos dois
> barramentos. O que não transfere é o modelo de conteúdo, e recalibrá-lo sem conteúdo realista
> piora em vez de ajudar. A calibração dos limites, que é a camada sem rótulo mais barata, corta o
> falso positivo em 56% e ainda assim fica duas ordens de grandeza acima do operável.

É um resultado negativo **com direção**, e a direção é verificável: se a ancoragem no CARLA trouxer o
falso positivo para perto da referência, a tese de "classificador congelado + calibração por domínio"
fica de pé com número. Se não trouxer, o modelo de conteúdo não transfere entre implantações de
SOME/IP — e isso também é publicável, desde que medido assim.

## Como reproduzir

```sh
# 1. barramento, duas rodadas: uma para calibrar, outra para medir
OUT=$PWD/platform/bus-kim/outA SUBNET=172.28.0.0/24 DUR=60 bash platform/bus-kim/run.sh
OUT=$PWD/platform/bus-kim/outB SUBNET=172.28.0.0/24 DUR=60 bash platform/bus-kim/run.sh

# 2. artefatos do treino -> formato do sensor
bash tools/build_sensor_artefacts.sh <dir-com-model_16.json>

# 3. calibrar as duas camadas nao supervisionadas na rodada A
python3 tools/calibrate_domain.py --repro <repo-de-reproducao> \
        --pcap platform/bus-kim/outA/bus-kim.pcap --out build/recal

# 4. medir na rodada B
./build/bin/eval_pcap platform/bus-kim/outB/bus-kim.pcap build/cfg   --engine compact
./build/bin/eval_pcap platform/bus-kim/outB/bus-kim.pcap build/recal --engine compact
```

⚠️ O `platform/bus-kim/run.sh` apaga `build/bin` para garantir compilação do zero, então o
`eval_pcap` tem de ser recompilado depois de cada rodada do barramento.

## Ressalvas

* **60 segundos por rodada**, ~108 mil pacotes, contra 2,19 M do benigno do dataset. A taxa é por
  pacote, então é comparável, mas a bancada vê menos variedade de regime.
* **A sub-rede é `172.28.0.0/24`**, não a `172.18.0.0/24` da matriz: desvio declarado, com o último
  octeto preservado (ver `platform/bus-kim/README.md`).
* Uma fração pequena dos eventos da bancada vem do **gateway do Docker** (`172.28.0.1`, serviço
  `0x0000`, algumas dezenas) e das nossas próprias ofertas de SD (`0xFFFF`). Não são o termo
  dominante e não mudam nenhuma das três leituras.
* A calibração **não verifica** que a janela é benigna — não há rótulo, e é esse o ponto. Aqui a
  garantia é por construção, porque nenhum atacante foi subido.
