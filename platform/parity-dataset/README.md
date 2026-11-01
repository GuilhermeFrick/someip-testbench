# Paridade sobre o dataset inteiro: 14,4 milhões de pacotes reais

```bash
docker build -f deploy/Dockerfile.parity -t someip-parity:1 .
PCAP_DIR=~/dataset/data/pcap bash platform/parity-dataset/run.sh
```

O [teste sintético](../../tests/README.md) compara 23 pacotes que **eu** escolhi, cobrindo os
casos de borda que eu soube imaginar. Este compara os 14,4 milhões do dataset, e portanto cobre o
que eu **não** soube imaginar.

A cadeia exercitada é a mesma que roda em produção:

```
PcapSource → SomeipParser → FeatureStreamer
```

Não existe caminho de código separado para o teste. É por isso que o `PcapSource` existe: sem ele
a paridade mediria um código que ninguém executa.

## Resultado

**14 375 392 quadros, 14 233 354 vetores de features, 0 divergências.** Comparação exata, bit a
bit no `float` de saída, sem tolerância.

| PCAP | quadros | descartados | vetores | fluxos | origens |
|---|---:|---:|---:|---:|---:|
| `benign_traffic` | 2 194 700 | 898 | 2 193 802 | 61 | 9 |
| `dos_noti_flood` | 1 931 593 | 67 063 | 1 864 530 | **37 349** | 10 |
| `fuzzy_sd_offer_rand_noti(1)` | 2 198 015 | 902 | 2 197 113 | 62 | 10 |
| `fuzzy_sd_offer_rand_noti(2)` | 1 305 398 | 1 244 | 1 304 154 | 9 512 | 10 |
| `fuzzy_sd_offer_rand_noti(3)` | 2 224 634 | 984 | 2 223 650 | 61 | 9 |
| `mitm_multi_attacker` | 2 413 810 | 1 281 | 2 412 529 | 8 615 | 11 |
| `mitm_single_attacker` | 2 107 242 | 69 666 | 2 037 576 | **43 289** | 10 |
| **total** | **14 375 392** | **142 038** | **14 233 354** | — | — |

Cada arquivo tem o seu extrator, porque é o que o `extract_file` do Python faz: o estado de janela
não atravessa a fronteira entre cenários.

Quatro invariantes são exigidas por arquivo, além da igualdade das features: mesmo número de
quadros lidos, mesmo número de quadros **descartados pelo parser**, mesmo número de vetores
emitidos, e `FlowEvictions()`/`SourceEvictions()` em zero.

## O que esta rodada achou, e que o teste sintético não acharia

### 1. O teto de fluxos estava dez vezes abaixo do necessário

`StreamerConfig::maxFlows` valia **4096**. O `mitm_single_attacker` tem **43 289** fluxos
distintos e o `dos_noti_flood` tem **37 349** — os dois randomizam portas, e cada porta nova é um
fluxo novo. Com o valor antigo o extrator despejaria dez vezes mais fluxos do que mantém.

O despejo **não é um erro visível**: o Python não tem teto, então no instante em que o despejo
dispara os dois lados param de calcular a mesma coisa, e as features continuam plausíveis. Foi o
contador de despejo, exigido em zero, que transformou isso num resultado em vez de num mistério.

O valor hoje é **65 536**, medido e não adivinhado, documentado no cabeçalho. É `maxFlows` que
dita a memória: a cerca de 400 bytes de estado por fluxo, a reserva custa uns 26 MiB. E nenhum
teto sobrevive a um atacante que continue inventando portas — por isso o contador fica.

### 2. O custo: a mesma travessia, 1255 s em Python e 6,3 s em C++

| | tempo |
|---|---:|
| referência em Python (7 arquivos) | **1255 s** |
| cadeia em C++ (7 arquivos) | **6,3 s** |

Cerca de **200×**, com os dois lados pagando a sua própria escrita e leitura dos 966 MB de
registros de referência. Isso dá 0,44 µs por quadro, que é um **teto** e não uma medida da cadeia:
o número inclui ler e comparar os registros. A medida limpa, por pacote, é a do banco de latência.

## Como a comparação é feita

O lado Python grava um registro por vetor emitido: `uint32` com o **índice do pacote** mais as 16
features em `float32`, little-endian, 68 bytes. São 966 MB.

O índice existe por um motivo prático: se um lado descartar um pacote que o outro aceitou, as
contagens divergem e **tudo depois disso parece errado**. Com o índice, o teste aponta o pacote
exato em vez de despejar um milhão de divergências.

`float32` e não `float64` porque o lado C++ produz `float`, e é nele que a comparação pode ser bit
a bit. Uma divergência menor que a resolução do `float32` não é detectada por aqui; a que importa,
de ordem de operações ou de janela, é.

## A imagem não tem vsomeip

A [`deploy/Dockerfile.parity`](../../deploy/Dockerfile.parity) é `ubuntu:24.04` com
`build-essential`, `libpcap-dev`, `scapy` e `numpy`. Esta cadeia não toca a pilha SOME/IP, e
compilar o vsomeip custaria dez minutos por nada. Quando existir um nó de sensor capturando no
barramento, o `libpcap-dev` passa a ser necessário também na `Dockerfile.vsomeip`.

As versões de `scapy` e `numpy` são fixas, pelo mesmo motivo da tag fixa do vsomeip: a rodada
precisa poder ser repetida.

## Pré-requisitos

- os 7 PCAPs do dataset (figshare), em `$PCAP_DIR`
- o repositório de reprodução ao lado, em `$REPRO`, pelos módulos de referência. Sem eles isto não
  é paridade, é a minha implementação concordando consigo mesma — e o `run.sh` para com essa
  mensagem em vez de rodar
- uns 2,5 GB livres: 1,4 GB de PCAP mais 966 MB de registros

```bash
MAX_PACKETS=50000 bash platform/parity-dataset/run.sh   # ensaio, ~30 s
```
