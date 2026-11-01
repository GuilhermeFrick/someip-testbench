# Plano por fases — someip-testbench

Cada fase é um **marco verificável** e reduz o risco da seguinte. Decisões travadas: repo próprio,
**malha aberta** (sem Autoware nesta etapa), nós de comunicação em **C++**, camada CARLA em Python.

A ordem é deliberada: primeiro provamos que a **pilha SOME/IP real funciona**, depois ancoramos na
**física**, depois **atacamos**, e só então ligamos a cadeia de **Security Event**. Cada fase produz
algo utilizável mesmo que a seguinte atrase.

---

## Fase 0 — Esqueleto da pilha

**Objetivo:** um provedor e um consumidor em C++ trocando um evento SOME/IP real, gerados a partir
da matriz de comunicação.

- Escrever o `platform/comm_matrix.yaml` mínimo: 1 serviço, 1 evento, 2 nós.
- `tools/gen_config.py`: matriz para config vsomeip e headers C++.
- `libecu`: `Skeleton` e `Proxy` mínimos sobre vsomeip.
- Docker Compose com a rede `bus` (multicast habilitado) e 2 containers.

**Critério de pronto:** o Wireshark mostra o **SOME/IP-SD** anunciando e descobrindo o serviço, e
as notificações sendo **decodificadas corretamente** pelo dissector.

**Risco:** multicast do SD dentro do Docker e corretude da serialização. São os dois pontos que
costumam travar, por isso vêm primeiro.

---

## Fase 1 — Bancada benigna ancorada no CARLA

**Objetivo:** tráfego SOME/IP realista produzido pela dinâmica de um veículo simulado.

- Nós: `gnss_ecu`, `imu_ecu`, `lidar_ecu`, `adas_ecu`, `hmi_ecu`.
- `carla_feeder.py` em modo síncrono, alimentando os sensores pela rede `ctrl`.
- `adas_ecu` consome os três sensores e publica estado de AEB (função derivada, com métodos além de eventos).
- `monitor` capturando a rede `bus`.

**Critério de pronto:** um PCAP **benigno** de alguns minutos, com pub/sub e request/response, em
que o padrão temporal do tráfego acompanha a condução (acelerações, curvas, frenagens).

**Risco:** desempenho do LiDAR (volume de dados). Mitigação: publicar uma nuvem reduzida ou apenas
o obstáculo mais próximo, que é o que o ADAS consome de fato.

---

## Fase 2 — Injeção de ataques e ground truth

**Objetivo:** dataset rotulado, gerado em ambiente vivo, no mesmo formato da primeira contribuição.

- `attacker_ecu` com as duas superfícies: via serviço (MITM, spoof) e on-the-wire (DoS, Fuzzy).
- Cenários em YAML, herdando a taxonomia e o `trigger_rate` do `someip-traffic-simulator`.
- `tools/label_pcap.py`: PCAP mais timeline mais mapa IP para papel, gerando rótulo por pacote.

**Critério de pronto:** o extrator `content_ext` consome o PCAP gerado **sem adaptação** e o
detector da primeira contribuição produz métricas por classe sobre ele.

**Risco:** viés sintético dos ataques (padrão temporal perfeito demais). Mitigação: jitter e
variação de taxa, como já previsto no gerador.

---

## Fase 3 — Cadeia Security Event / IdsM

**Objetivo:** fechar o elo que o artigo deixou como trabalho futuro: a classe predita vira um
Security Event tipado, consumido por um IdsM.

São quatro frentes, em ordem de esforço:

1. **Extrator das 16 features em C++** (o caminho crítico). Tem estado: janelas deslizantes por
   fluxo e por origem, *byte-models*, Hamming entre *payloads*. Já nascer com a *source diversity*
   incremental O(1).
2. **Exportar os dados que o extrator precisa**: tabelas de log-probabilidade dos *byte-models* e os
   vetores `xmin`/`xmax` da normalização.
3. **Modelo via TL2cgen**, compilado junto no build do modelo selecionado.
4. **`libidsm`**: mapear a classe para `SecurityEventDefinition` e reportar.
- `libidsm`: `EventReporter::ReportEvent(...)` e o IdsM com cadeia de filtros (taxa, agregação, limiar).
- Reporter: log estruturado e, opcionalmente, um serviço SOME/IP representando o SOC.

**Critério de pronto:** dois testes de paridade passando, e um cenário de ataque produzindo um
**log de eventos qualificados** alinhado temporalmente com a janela do ataque injetado.

**Paridade é critério de aceite, em dois níveis:**

- *Features*: o mesmo PCAP produz as **mesmas 16 features** em Python e em C++. Esse é o teste que
  pega os erros sutis (janela que começa cheia, ordem de atualização diferente).
- *Classe*: a predição em C++ bate com a do `predict_proba`. A `libxgboost` serve de oráculo.

**Ganho colateral:** a inferência cai de cerca de **0,5 ms** por chamada, medidos na primeira
contribuição, para **1,39 µs**. A causa não era a fronteira Python, que respondia por cerca de 25%,
nem o caminho de uma linha da `libxgboost`: era o **tamanho do modelo**. A mesma biblioteca caiu de
533 para 51 µs só trocando o modelo. Medição em `experiments/bench-inference/RESULTADOS.md`.

**Segundo oráculo:** o **avaliador compacto** (modelo como tabelas `const` percorridas por laço
limitado) é **2x mais lento** que o TL2cgen com o modelo adotado, então não entra no caminho quente.
Mas tem **paridade exata** (0,0 contra 3,87e-07) e **WCET limitado por construção**, o que o torna um
segundo oráculo independente, ao lado da `libxgboost`. Não é papel menor: foi a conferência cruzada
que revelou que o `base_score` deste modelo é um vetor por classe, e não um escalar, erro que
passaria silencioso. Evidência em `experiments/bench-inference/RESULTADOS.md`.

---

## Fase 4 — Empacotamento como ferramenta

**Objetivo:** virar artefato de pesquisa reutilizável e citável.

- CLI única: descrever cenário, subir a bancada, capturar, rotular e avaliar.
- Docker Compose completo, documentação de uso e um exemplo reprodutível de ponta a ponta.
- Modos de uso: **gerar dataset**, **testar um modelo de IDS**, **rodar um cenário de ataque**.

**Critério de pronto:** alguém que não participou do projeto reproduz um dataset rotulado seguindo
apenas o README.

---

## Riscos transversais

| Risco | Mitigação |
|---|---|
| Multicast do SD não funcionar no Docker | Validar na Fase 0, antes de qualquer outra coisa; alternativa é rede `macvlan` |
| Serialização SOME/IP incorreta | O dissector do Wireshark é o critério de aceite, desde a Fase 0 |
| Alinhamento temporal entre ataque, captura e detecção | Clock único do CARLA em modo síncrono, carimbado em todos os componentes |
| Escopo inflar rumo a "reimplementar o CANoe" | O escopo é SOME/IP, sem GUI, sem CAPL, sem hardware; está declarado no README |
| Tráfego de controle contaminar o dataset | Duas redes Docker separadas, com a captura restrita à rede `bus` |

---

## ✅ Fase 0 concluída

| Item da Fase 0 | Estado |
|---|---|
| `platform/comm_matrix.yaml` mínimo | feito |
| `tools/gen_config.py`: matriz para config vsomeip, rótulos e **cabeçalhos C++** | feito |
| SD anunciando e descobrindo entre containers | validado em [`platform/sd-smoke`](../platform/sd-smoke/README.md) |
| `libecu`: `Skeleton` e `Proxy` | feito, sobre vsomeip 3.6.4 |
| Dois nós nossos trocando um evento decodificado pelo dissector | **passou**, em [`platform/node-handshake`](../platform/node-handshake/README.md) |
| `deploy/docker-compose.yml` com a rede `bus` | a escrever |

**Critério de pronto atendido.** 22 notificações em 20 s, e os campos no barramento batem com a
matriz: `service 4660` (0x1234), `event 1911` (0x8777), `msgtype NOTIFICATION`. Os dois lados
concordam no conteúdo, com o contador do provedor chegando igual no consumidor.

Os dois riscos que justificavam a ordem das fases **estão retirados**: o multicast do SD dentro do
Docker (vira requisito, `ip route add 224.0.0.0/4` mais `NET_ADMIN` em cada nó) e a corretude da
serialização (o dissector decodifica, com os identificadores da matriz).

O `docker-compose.yml` fica para a Fase 1, onde passa a haver cinco nós e duas redes. Com dois nós
o `run.sh` do degrau basta, e um compose agora seria reescrito na fase seguinte.

## ✅ Caminho crítico da Fase 1 vencido: a cadeia de extração, com paridade

O caminho crítico da Fase 1 não eram os nós — esses são variações do `gnss_ecu`, cujo molde já
está provado. Era o **sensor em C++**, porque a paridade com o Python se ganha ou se perde nele, e
**em silêncio**: se as features divergirem, o modelo treinado lá não vale aqui e nada acusa.

| Estágio | Estado |
|---|---|
| `ByteModel.cpp` — pontuação **posicional** e Hamming | feito |
| `FeatureStreamer.cpp` — o único estágio com estado | feito |
| `SomeipParser.cpp` — Ethernet → IP → TCP/UDP → SOME/IP → SD | feito |
| `PacketSource.cpp` — replay de PCAP e captura viva, sobre libpcap | feito |
| `Normalizer.cpp` — limites min-max do treino | feito |
| `Classifier.cpp` — TL2cgen e o avaliador compacto | feito, os dois conferidos contra o XGBoost |
| `SecurityEventDispatcher.cpp` — um reporter `ara::idsm` por classe | feito |
| `Sensor.cpp` — a cadeia e a decomposição de custo | feito |

**Os quatro estágios restantes foram fechados em 01/11, e com eles o `src/ara` ganhou
implementação** (`InstanceSpecifier`, `CoreErrorDomain`, `EventReporter`). O `ids_sensor` tem hoje
**nove `.cpp`** e nenhum cabeçalho sem implementação.

**Critério de pronto atendido, e com margem:** a cadeia de produção inteira
(`PcapSource → SomeipParser → FeatureStreamer`) foi rodada sobre os 7 PCAPs do dataset e comparada
com o código Python que treinou o modelo. **14 375 392 quadros, 14 233 354 vetores, 0
divergências**, exato bit a bit. Ver [`platform/parity-dataset`](../platform/parity-dataset/README.md).

Dois achados que só uma rodada desse tamanho dava:

1. **o teto de fluxos estava dez vezes abaixo do necessário.** `maxFlows` valia 4096, e os
   arquivos que randomizam portas chegam a 43 289 fluxos distintos. O despejo não é erro visível —
   o Python não tem teto, então a partir dele os dois lados calculam coisas diferentes com
   aparência plausível. Foi o contador de despejo, exigido em zero, que virou isso num resultado;
2. **custo:** a mesma travessia leva 1255 s em Python e **6,3 s** em C++, uns 200×.

## Próximo passo imediato

O componente está completo e testado, mas **falta o artefato que o torna o IDS de verdade: o
modelo treinado.** O `model_16.json` e o `minmax.npy` da rodada de treino não estão versionados em
nenhum repositório e estavam só na VM, que foi desligada. Hoje o `Classifier` roda contra um
**modelo de prova** treinado em dados aleatórios, que valida travessia, ABI e layout — mas não é o
modelo adotado.

Duas frentes, nesta ordem:

1. **Recuperar ou regerar os artefatos de treino**, e versioná-los junto do repositório de
   reprodução. Os 14,2 M vetores de features já estão no disco da bancada, então falta o rótulo
   por pacote para retreinar. É também o que fecha o `TrainingRunMismatch`, o último código de erro
   declarado que nada produz: ele exige uma marca de rodada nos três artefatos.
2. **O nó de sensor** (`src/nodes/`), que é o `Sensor` dentro de um executável com `LiveSource` no
   barramento — e aí o `libpcap-dev` na imagem da bancada, que já foi acrescentado, passa a ser
   usado para o que motivou acrescentá-lo. Atenção ao achado do `sd-smoke`: um monitor passivo em
   container separado **não vê o multicast**, então o nó precisa entrar nos grupos IGMP.
