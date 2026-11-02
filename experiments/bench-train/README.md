# Treinar o detector com dados da bancada

**Pergunta:** o falso positivo alto da bancada é limitação do **modelo** da C1 ou do **dado** com que
ele foi treinado (o barramento do Kim, com o carro quase sempre parado)?

**Método:** a receita da C1 sem mudança nenhuma (16 features `content_ext`, modelos de bytes
ajustados no benigno, normalização min-max, XGBoost com 25 rodadas e profundidade 8), importada do
repositório de referência. Só mudam os dados e o rótulo:

* **dados:** 10 rodadas vivas da bancada (`collect.sh`), benigno e os quatro ataques, cada cenário
  duas vezes: uma rodada de **treino** (120 s) e outra, independente, de **teste** (60 s). Conteúdo
  benigno `counter`. Treino: 992 002 pacotes; teste: 512 492.
* **rótulo:** o MAC de quem pôs o pacote no fio.
* **avaliação 2 × 2:** o modelo do Kim (artefatos da C1) e o modelo da bancada, cada um testado na
  bancada, no Kim (últimos 30% de cada PCAP, o teste temporal da C1) e num benigno com conteúdo novo
  (VDE com perfil sintético de carro andando, `platform/scenario-live`).

```sh
nohup bash experiments/bench-train/collect.sh > /tmp/collect.out 2>&1 &     # ~25 min
# na imagem someip-parity:1 (xgboost, scapy):
python3 experiments/bench-train/train_bench.py --repro <C1> --runs experiments/bench-train/runs \
    --kim-art <artefatos-C1> --kim-pcap <pcaps-Kim> \
    --novelty platform/scenario-live/out-none-sintetico --out experiments/bench-train/out
python3 experiments/bench-train/fp_breakdown.py ...                          # FP por rodada e origem
```

As métricas são sobre os pacotes que o parser aceita (o detector não classifica IGMP nem quadros que
não são IPv4); por isso diferem das de `experiments/eval-pcap/DETECCAO.md`, que conta todos os quadros.

## Resultado: as 16 features

| | modelo do Kim | modelo da bancada |
|---|---|---|
| **bancada:** falso positivo | 6,4% | **5,1%** |
| recall DoS / Fuzzy / MITM single / MITM multi | 0,99 / 1,00 / 0,82 / **0,02** | 0,92 / **0,00** / 0,97 / **0,74** |
| **Kim:** falso positivo | 1,4% | **42,1%** |
| recall DoS / Fuzzy / MITM single / MITM multi | 1,00 / 1,00 / 1,00 / 0,71 | 0,87 / 0,00 / 0,70 / 0,18 |
| **carro andando:** falso positivo | 13,8% | **30,1%** |

Falso positivo por rodada de teste e por origem (`fp_breakdown.py`):

| rodada | modelo do Kim | modelo da bancada | origem principal do FP (modelo da bancada) |
|---|---:|---:|---|
| só benigna | 8,9% | 4,2% | IMU, `0x1002` (3 740 de 4 311) |
| DoS | 1,8% | 6,3% | IMU, `0x1002` |
| Fuzzy | 8,7% | 3,8% | IMU, `0x1002` |
| MITM single | 1,7% | 5,4% | IMU, `0x1002` |
| MITM multi | 8,5% | 6,9% | IMU e VDE |

## Leitura

1. **Treinar no domínio certo corrige a família que não transferia:** MITM multi sobe de 0,02 para
   0,74, e o MITM single de 0,82 para 0,97.
2. **Mas não resolve o falso positivo.** Dentro do próprio domínio o modelo da bancada fica em 5,1%,
   e o falso positivo existe igual na rodada **sem ataque nenhum** (4,2%): não é reação das vítimas ao
   ataque.
3. **O falso positivo vem sempre do conteúdo de uma ECU sensora:** a VDE no modelo do Kim, a IMU no da
   bancada. O alvo troca, o mecanismo é o mesmo.
4. **A transferência quebra nos dois sentidos:** o modelo da bancada no Kim tem 42% de falso positivo,
   tão ruim quanto o do Kim na bancada; e com conteúdo novo (carro andando) é ainda mais sensível
   (30%).
5. **O Fuzzy da bancada não foi aprendido** (recall 0): 435 pacotes de Fuzzy no treino contra 880 mil
   normais.

**Conclusão:** o problema não é só o dado, é o **tipo de feature**. 11 das 16 features dependem do
conteúdo do payload (modelos de bytes, entropia, mudança entre payloads, `repeat_rate`,
`src_payload_div`); só 5 são livres de conteúdo (intervalo de tempo e quatro de tamanho). E nenhuma
descreve o comportamento do Service Discovery, embora os quatro ataques do dataset sejam ataques de SD.

## Ablação: só as 5 features livres de conteúdo

A mesma receita e os mesmos dados, com o modelo da bancada treinado só em `ip_time_interval`,
`ip_len_change`, `l4_len_change`, `someip_len` e `l4_len` (`--features`; artefatos em `out-5feat/`).

| | bancada, 16 features | **bancada, 5 features sem conteúdo** | Kim, 16 features |
|---|---:|---:|---:|
| **falso positivo na bancada** | 5,1% | **0,31%** | 6,4% |
| recall DoS / Fuzzy / MITM single / MITM multi | 0,92 / 0,00 / 0,97 / 0,74 | **0,85 / 0,85 / 0,69 / 0,76** | 0,99 / 1,00 / 0,82 / 0,02 |
| precisão nas 4 famílias | 0,57 a 0,97 | **0,88 a 0,97** | 0,16 a 0,94 |
| **falso positivo com carro andando** | 30,1% | **0,16%** | 13,8% |
| **falso positivo no Kim** | 42,1% | **3,6%** | 1,4% (domínio dele) |
| recall no Kim: DoS / Fuzzy / MITM single / MITM multi | 0,87 / 0,00 / 0,70 / 0,18 | **0,80 / 0,86 / 0,98 / 0,20** | 1,00 / 1,00 / 1,00 / 0,71 |

**Leitura.** Tirar as features de conteúdo:

* derruba o falso positivo **16×** dentro do domínio (5,1% → 0,31%);
* elimina a sensibilidade a conteúdo novo (carro andando: 30% → 0,16%);
* faz o modelo **transferir para o dataset do Kim sem tê-lo visto** (falso positivo 3,6%; MITM single
  0,98, Fuzzy 0,86, DoS 0,80);
* e ainda detecta as quatro famílias na bancada, com precisão de 0,88 a 0,97 (o Fuzzy, que as 16
  features não aprenderam, passa a 0,85).

**O conteúdo do payload era a causa tanto do falso positivo quanto da falta de transferência.** As
features de conteúdo fazem do detector um detector de novidade de bytes preso ao domínio de treino;
as estruturais (tempo e tamanho) descrevem o tráfego, não o que ele carrega.

O que ainda não é bom: MITM multi no Kim (0,20) e MITM single na bancada (0,69). Nenhuma das 5
features descreve o Service Discovery (ofertas por origem, mais de um provedor por serviço, StopOffer
de quem não é o provedor), e é por SD que os quatro ataques acontecem.

## Checagem simétrica: treino no Kim, teste na bancada (`train_kim.py`)

Mesmo split da C1 (primeiros 70% de cada PCAP do Kim para treino, últimos 30% para teste), rótulo
pelo MAC; modelos de bytes no PCAP benigno inteiro, como a C1.

| modelo treinado no Kim | FP no Kim | **FP na bancada** | FP carro andando | Kim: DoS / Fuzzy / MITM s / MITM m | bancada: DoS / Fuzzy / MITM s / MITM m |
|---|---:|---:|---:|---|---|
| 16 features, rótulo MAC (`c1_mac`) | **0,86%** | 5,4% | 0,65% | 1,00 / 1,00 / 1,00 / **0,99** | 0,95 / 1,00 / 0,85 / 0,01 |
| 5 sem conteúdo | 1,5% | **0,05%** | **0,04%** | 0,75 / 1,00 / 0,99 / 0,25 | 0,60 / 1,00 / 0,69 / 0,25 |

**Leitura.**

1. **A transferência das features sem conteúdo vale nos dois sentidos.** Treinado só no Kim, o
   modelo de 5 features tem 0,05% de falso positivo na bancada, contra 5,4% das 16 features. Com o
   sentido inverso (treino na bancada, 3,6% no Kim), o resultado fica completo.
2. **Corrigir o rótulo melhora a própria C1.** Com o rótulo pelo MAC, os 163 188 `StopOffer`
   forjados entram no treino como ataque, e o recall do MITM multi no Kim sobe de 0,71 (artefatos da
   C1, rótulo por IP) para **0,991**. Dentro do domínio, a C1 corrigida fica com 0,86% de falso
   positivo e todas as famílias acima de 0,99.
3. **O dilema:** dentro do domínio as 16 features vencem; fora dele só as 5 se sustentam, mas detectam
   pouco o MITM multi (0,25) e medianamente o DoS (0,60 a 0,75). É o buraco que as features de SD
   (`sd_features.py`) foram desenhadas para cobrir.
