# Detecção na bancada: só o Fuzzy transfere

O `eval_pcap` mede **falso positivo** sobre tráfego benigno (ver `RESULTADOS.md`).
Este experimento mede a outra metade — **detecção** — injetando os quatro padrões
adversariais do baseline sobre a captura da bancada e pontuando a cadeia congelada
contra o ground truth.

* injeção: `tools/inject_scenarios.py` — assinaturas (serviço, transporte, tipo,
  cadência, densidade) **medidas do dataset real**, não de um gerador;
* pontuação: `experiments/eval-pcap/score_pcap.cpp` — alinha predição×rótulo por
  índice de frame via `OnFrame`, e dá recall/precisão/F1 por classe.

## O resultado

Detector congelado (treinado no dataset do Kim), sobre a reprodução da bancada:

| classe | recall | precisão | F1 |
|---|---:|---:|---:|
| **Fuzzy** | **0,586** | 0,414 | 0,485 |
| MITM-multi | 0,032 | 0,861 | 0,061 |
| MITM-single | 0,010 | 0,028 | 0,015 |
| DoS | 0,000 | — | 0,000 |

## O controle que torna isso um resultado, e não um bug

O mesmo modelo e o mesmo `score_pcap`, sobre os **PCAPs reais** do dataset
(rotulados por origem ∉ benigna), reproduzem o desempenho publicado:

| classe | recall **real** | recall **bancada** | queda |
|---|---:|---:|---:|
| DoS | 0,771 | 0,000 | total |
| Fuzzy | 1,000 | 0,586 | parcial |
| MITM-single | 0,986 | 0,010 | total |
| MITM-multi | 0,979 | 0,032 | total |

O scorer detecta os ataques reais (macro-F1 ≈ 0,96, batendo com a C1). Logo os
números baixos da bancada são **transferência de domínio genuína**, não defeito de
medição.

## A leitura

**Só o Fuzzy transfere, e mesmo ele degrada (1,00 → 0,59).** A razão é a mesma que
explica o falso positivo do `RESULTADOS.md`, agora do lado do ataque:

> o `content_ext` detecta os ataques do dataset pela IMPRESSÃO DE CONTEÚDO — a
> distribuição de bytes daquela captura específica — e não pela estrutura
> comportamental do ataque.

Fuzzy é a exceção porque sua assinatura **é** conteúdo aleatório, e aleatório é
aleatório em qualquer barramento — a feature discriminante é invariante ao domínio.
DoS, MITM-single e MITM-multi têm comportamento distinto (rajada, replay, relay
0x100B), mas no dataset a discriminação vinha de bytes incidentais daquele tráfego;
reproduzidos na bancada com o conteúdo da bancada, esse sinal some.

É o **espelho** do achado benigno. Juntos:

> o `content_ext` está fortemente acoplado às distribuições de bytes do domínio de
> treino, para tráfego benigno **e** de ataque. Um detector reportado a macro-F1
> 0,96 no dataset detecta 3 de 4 classes de ataque via artefato de conteúdo
> específico daquele dataset, não via estrutura de ataque transferível.

## O que o experimento mede, e o que não mede

Mede: a reprodução fiel do COMPORTAMENTO do ataque (serviço, transporte, densidade,
cadência) é detectada pelo detector congelado, quando o CONTEÚDO é o da própria
reprodução?

Não mede, e não poderia sem ser circular: o conteúdo de byte **exato** do dataset.
Injetar os bytes do Kim seria copiar o dataset, não reproduzir um barramento. O
DoS e o MITM da bancada copiam o payload legítimo da bancada (comportamento certo,
conteúdo da bancada); o Fuzzy usa conteúdo aleatório, como o ataque real.

⚠️ Isto **não** prova que o detector é ruim. Prova que a discriminação de DoS/MITM
no dataset do Kim veio, em boa parte, de features de conteúdo específicas do
domínio. Um detector ancorado em features comportamentais/temporais poderia
transferir melhor — é uma hipótese que a bancada agora permite testar.

## Como reproduzir

```sh
# injetar cada cenário sobre a captura benigna da bancada
for S in dos fuzzy mitm_single mitm_multi; do
  python3 tools/inject_scenarios.py --repro <repo-reproducao> \
          --pcap platform/bus-kim/out/bus-kim.pcap --scenario $S --out build/scn/$S
done

# pontuar a cadeia congelada contra o ground truth
for S in dos fuzzy mitm_single mitm_multi; do
  ./build/bin/score_pcap build/scn/$S.pcap build/scn/$S.labels build/cfg --engine compact
done
```

Controle (precisa dos PCAPs do dataset): `tools/label_real.py` rotula por origem,
e o mesmo `score_pcap` pontua.

## Ressalvas

* as frações de ataque injetadas casam com as medidas no dataset (DoS 15,3%,
  Fuzzy 0,2%, MITM-single 19,5%, MITM-multi 17,2%) — é reprodução de densidade,
  não ajuste;
* a precisão alta do MITM-multi (0,861) sobre recall baixo (0,032) diz que, quando
  o detector dispara `mitm-multi`, quase sempre acerta — mas dispara raramente;
* o recall de `normal` na bancada (~0,91) carrega os mesmos ~9000 falsos positivos
  do `RESULTADOS.md`: o detector já está confuso no benigno da bancada, o que é
  parte do mesmo fenômeno.
