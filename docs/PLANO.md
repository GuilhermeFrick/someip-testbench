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

- `ids_sensor` em C++ linkando a **`libxgboost` pela C API** (`XGBoosterLoadModel` mais
  `XGBoosterPredictFromDense`), com **`nthread=1`** para a medição em 1 núcleo ficar comparável.
- Extração das 16 features em C++ mais mapeamento da classe para `SecurityEventDefinition`.
- `libidsm`: `EventReporter::ReportEvent(...)` e o IdsM com cadeia de filtros (taxa, agregação, limiar).
- Reporter: log estruturado e, opcionalmente, um serviço SOME/IP representando o SOC.

**Critério de pronto:** rodar um cenário de ataque e obter um **log de eventos qualificados**
alinhado temporalmente com a janela do ataque injetado.

**Ganho colateral:** chamar o motor nativo direto remove o gargalo de cerca de 0,5 ms por chamada,
medido na primeira contribuição, que era o custo da fronteira Python. É a mesma implementação
servindo às duas frentes.

**Risco: baixo.** Como é o mesmo motor que treinou o modelo, não há divergência numérica a
verificar, e o `multi:softprob` de 5 classes é tratado nativamente. Ainda assim, vale confirmar que
a classe predita em C++ bate com a do `predict_proba` numa amostra do conjunto de teste.

**Fora de escopo nesta fase:** exportar o modelo para C embarcável (tabelas `const` mais avaliador).
Isso só importa se formos para ECU real; a comparação com a `libxgboost` fica para depois, já com o
número da bancada em mãos.

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

## Próximo passo imediato

**Fase 0.** Antes de qualquer código de nó, escrever a `comm_matrix.yaml` mínima e validar que o
SOME/IP-SD sobe e é decodificado no Wireshark dentro do Docker. É o único risco que, se não for
resolvido, invalida o resto da arquitetura.
