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

**Ganho colateral:** a inferência nativa remove o gargalo de cerca de 0,5 ms por chamada medido na
primeira contribuição, que vinha da fronteira Python somada ao caminho de uma linha da
`libxgboost`.

**Melhoria reservada:** o **avaliador compacto** (modelo como tabelas `const` percorridas por laço
limitado) já foi medido em **2,2x mais rápido que o TL2cgen**, com paridade exata, 4,5x menos bytes
e sem recompilar para trocar de modelo. Fica guardado como evolução, com o número já em mãos. Antes
de promovê-lo é preciso fechar a lacuna dos **valores ausentes**: ele ignora a direção `missing` do
XGBoost, o que não afetou a paridade porque os dados não têm NaN. Evidência em
`experiments/bench-inference/RESULTADOS.md`.

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
