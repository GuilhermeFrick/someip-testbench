# Detecção com os ataques reais, ao vivo no barramento

Fase C de `docs/PLANO.md`. Mede recall, precisão e falso positivo do detector congelado da C1
(XGBoost de 5 classes, 16 features `content_ext`) sobre rodadas **vivas** da bancada, em que os
quatro ataques do dataset do Kim acontecem no barramento (`platform/scenario-live`).

## Como foi medido

* **Ataques:** ECUs injetoras que reproduzem o mecanismo medido no dataset: ofertas falsas no
  Service Discovery, `StopOffer` em nome da VDE (com IP forjado no MITM multi), vítimas abrindo o
  TCP com o atacante. As assinaturas batem com o dataset (`platform/scenario-live/README.md`).
* **Rótulo:** o MAC de quem pôs cada quadro no fio (`tools/label_by_mac.py`). O IP de origem não
  serve: o MITM multi forja o da VDE.
* **Pontuação:** `score_pcap`, alinhando predição e rótulo por índice de quadro.
* **Controle:** o mesmo detector, o mesmo `score_pcap` e a **mesma regra de rótulo** sobre os PCAPs
  reais do dataset, cujos atacantes têm MAC conhecido.
* Rodadas de 60 s, vsomeip 3.5.6, payload benigno `counter`. Script:
  `bash experiments/eval-pcap/score_live.sh`.

## Resultado

| ataque | recall dataset | recall bancada | precisão dataset | precisão bancada | macro-F1 dataset | macro-F1 bancada |
|---|---:|---:|---:|---:|---:|---:|
| DoS | 0,635 | **0,620** | 1,000 | 0,999 | 0,868 | 0,857 |
| Fuzzy | 1,000 | **0,974** | 0,987 | 0,424 | 0,997 | 0,772 |
| MITM single | 0,828 | **0,645** | 1,000 | 0,937 | 0,943 | 0,856 |
| MITM multi | 0,708 | **0,019** | 0,914 | 0,857 | 0,871 | 0,450 |

Falso positivo sobre os quadros benignos (1 − recall de `normal`): no dataset 0,01% a 0,03% (2,2%
no MITM multi); na bancada **1,6% a 9,0%**.

## Leitura

**1. Com o mecanismo real, a detecção transfere em três das quatro famílias.** DoS (0,62 contra
0,64) e Fuzzy (0,97 contra 1,00) transferem quase inteiros; MITM single transfere em parte (0,64
contra 0,83), com precisão alta. Isto **inverte** o resultado anterior desta página ("só o Fuzzy
transfere"), que vinha de ataques colados no PCAP e não reproduzia o mecanismo do dataset.

**2. O que não transfere é o tráfego benigno.** O falso positivo da bancada fica entre 1,6% e 9,0%,
contra ~0,01% no dataset. É o mesmo achado de `RESULTADOS.md`: o conteúdo do payload benigno da
bancada (contador sintético) não é o do dataset, e o detector classifica por conteúdo. A precisão
de 0,42 do Fuzzy é esse efeito: o ataque tem só 230 quadros, e os falsos positivos benignos pesam
mais que ele. É o alvo do CARLA (fase G).

**3. O MITM multi não transfere, e por confusão de família.** Dos 24 445 quadros de ataque, 5 946
saem como MITM **single** e só 457 como multi. O repasse do .14 copia o payload da VDE, e o payload
da VDE na bancada é o contador sintético: o conteúdo que distingue as duas famílias no dataset não
está aqui.

**4. A regra de rótulo do dataset esconde parte do MITM multi, e o detector herdou isso.** O
atacante .15 manda à ADAS 163 188 `StopOffer` com o IP da VDE forjado. A regra por IP do dataset
os rotula como tráfego normal, e o detector, treinado com esses rótulos, classifica **todos os
163 188 como normais** (recall 0,000, pontuando só esses quadros). É por isso que o recall do MITM
multi no próprio dataset cai de 0,979 (regra por IP) para 0,708 (regra por MAC): a parte mais
agressiva do ataque nunca foi ensinada como ataque. Isto vale para a C1, que treinou com o rótulo
do dataset.

## Diferenças entre as duas regras de rótulo no dataset

| PCAP | quadros de ataque pelo MAC | quadros que a regra por IP rotula diferente |
|---|---:|---:|
| `dos_noti_flood` | 372 821 | 0 |
| `fuzzy_sd_offer_rand_noti(1)` | 5 245 | 0 |
| `mitm_single_attacker` | 429 123 | 0 |
| `mitm_multi_attacker` | 591 007 | **163 188** (StopOffer com IP da VDE) |

## Fase B, que esta medição pressupõe

O `ids_sensor_node` rodou ao vivo nas mesmas rodadas, escutando na interface da bridge (o
equivalente a uma porta espelhada do switch), e emitiu os security events dentro da janela de ataque:
10 159 `DenialOfService` vindos do injetor do DoS, 224 `MalformedPayload` do Fuzzy (de 230 quadros),
15 187 `ManInTheMiddleSingle` do MITM single, primeiro evento entre 2 e 300 ms depois da entrada do
injetor (`tools/events_timeline.py`).

## Histórico

A versão anterior desta página (até o commit `b47314b`) media detecção sobre rajadas coladas no
PCAP depois da captura, rotuladas por IP, e concluía que só o Fuzzy transferia. Foi invalidada
quando `tools/attack_flows.py` mostrou que os ataques do dataset são de Service Discovery, com as
vítimas abrindo o TCP com o atacante, o que só acontece num barramento vivo.
