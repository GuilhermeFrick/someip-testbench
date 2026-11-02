# Plano por fases — someip-testbench

Reescrito em **2026-11-02**. A versão anterior colocava o CARLA como próximo passo e tratava os
ataques como injeção genérica. Três fatos mudaram a ordem:

1. a bancada já **reproduz o barramento do Kim** (9 ECUs, um binário, matriz recuperada do dataset);
2. os ataques do dataset foram **medidos no fio** e são ataques de **Service Discovery** feitos por
   aplicações vsomeip, não rajadas coladas (`tools/attack_flows.py`);
3. a injeção pós-captura **não reproduz esse mecanismo**, e o resultado de detecção obtido com ela
   foi invalidado (`experiments/eval-pcap/DETECCAO.md`).

## Decisões travadas

- **O ataque acontece no barramento vivo.** Cada atacante é uma ECU com IP próprio, rodando vsomeip,
  e só faz o que uma ECU comprometida faria seguindo o SOME/IP. Nada que a pilha real não permita.
- **Os atacantes reproduzem o que o dataset contém**, medido no PCAP: mecanismo, IPs, transporte.
- **O IDS lê o fio** (captura na bridge e sensor nos grupos multicast), nunca um log escrito pelos nós.
- **Rótulo pelo MAC do container atacante.** O IP de origem não basta: no MITM multi do dataset o
  atacante forja o IP da VDE (100 930 `StopOffer`), e a regra por IP do dataset rotula esses quadros
  como normais. A bancada registra também o que a regra por IP daria, para comparar com o dataset.
- **Duas redes**: `bus`, capturada, e `ctrl`, nunca capturada. Todo comando de controle passa pela `ctrl`.
- **CARLA por último.** Ele troca o conteúdo do payload, não a estrutura da bancada.
- **Interface gráfica entra no escopo** (antes declarada fora): modelar, controlar e observar, como o
  CANoe, mas só depois que exista algo vivo para mostrar.

## O barramento de referência

| IP | ECU | papel | serviço / assinaturas |
|---|---|---|---|
| .2 | ADAS | consumidor | VDE, IMU, GPS |
| .3 | CLU | consumidor | VDE |
| .4 | GPS | provedor | 0x1003 |
| .5 | IMU | provedor | 0x1002 |
| .6 | NAV | consumidor | IMU, GPS |
| .7 | PT | consumidor | VDE |
| .8 | STE | consumidor | IMU |
| .9 | TEL | consumidor | VDE, IMU, GPS |
| .10 | VDE | provedor | 0x1001 |

Identidade recuperada por conteúdo (`tools/decode_payload_semantics.py`) e confirmada pela ordem
alfabética dos IPs. A figura 2 do artigo do Kim difere do PCAP em duas ligações (ADAS também recebe
GPS; PT não recebe IMU). Vale o PCAP.

## Os ataques, como estão no dataset

| ataque | atacante | mecanismo medido |
|---|---|---|
| DoS | .11 | oferta falsa de 0x1001 **a cada 20 ms**; ADAS, CLU, PT e TEL assinam e **abrem o TCP** para o atacante; inundação de notificações pela conexão. UDP 59% (todo SD), TCP 41% |
| Fuzzy | .17 | só `OfferService` de ~400 serviços aleatórios (0x20xx), com endpoint próprio. 100% UDP, sem notificação |
| MITM single | .13 | oferta falsa de 0x1001 a cada 10 ms e `StopOffer` em nome da VDE direto à ADAS; só a ADAS assina e conecta nele. UDP 80% (todo SD), TCP 20% |
| MITM multi | .14 + .15 | .14 assina a VDE como cliente, repassa os eventos pelo serviço 0x100B ao .15; o .15 oferece 0x1001 e manda à ADAS `StopOffer` com o **IP da VDE forjado**; a ADAS conecta no .15. 98% TCP. Dois atacantes |

---

## Fase A — ECUs atacantes vivas

**Objetivo:** os quatro ataques acontecendo no barramento, como aplicações vsomeip sobre o
`NodeRuntime`, com os IPs do dataset.

**A0. Riscos, testados antes de qualquer código de ataque:**

| risco | como testar | se falhar |
|---|---|---|
| o vsomeip 3.6.4 aceitar dois nós oferecendo o mesmo serviço e instância | dois provedores de 0x1001 e um consumidor, em 3 containers | investigar a versão usada no dataset; ajustar a configuração |
| o payload das notificações do atacante (cópia da VDE ou conteúdo próprio) | estender `attack_flows.py` para comparar os bytes com os da VDE | nenhum: só define o que o atacante publica |
| o passo do MITM single em que o atacante manda à ADAS uma oferta com o endpoint da VDE | ver se o vsomeip produz isso sozinho; senão, montar o quadro SD à mão | quadro SD válido montado fora da API |

**Resultado da A0 (02/11, `platform/dup-offer`):**

- risco 1: **resolvido com o vsomeip 3.6.4.** Com o atacante entrando depois da VDE, as vítimas
  assinam nele mas mantêm o TCP com a VDE (a oferta a cada 20 ms só derruba a conexão legítima,
  2 059 vezes em 45 s). Isso é o que acontece com CLU, PT e TEL no MITM single do dataset. Com o
  atacante no ar **antes** da VDE, como no DoS do dataset, as vítimas migram para ele na mesma
  sequência do PCAP. 3.5.6 e 3.1.20.3 não mudam o quadro; a causa era a ordem de subida;
- risco 3: o passo é um `StopOffer` em nome da VDE (do IP do atacante no MITM single, com IP forjado
  no MITM multi). Não sai da API do vsomeip: o atacante precisa de um **emissor de SD bruto**;
- risco 2 (payload do atacante, `tools/attacker_payload.py`, comparando pelo MAC): o atacante **não copia**
  a VDE ao publicar para as vítimas. DoS: um único payload constante de 24 B no evento 0x0001 (51 950
  mensagens). MITM single: valores próprios plausíveis, 10 933 distintos, nenhum igual aos da VDE.
  MITM multi: o .15 manda à ADAS um payload constante; o relay 0x100B (.14 → .15) é **cópia exata** da
  VDE (100% idênticos). No PCAP do DoS a VDE não publica nada no trecho analisado: as vítimas migraram.

**A1–A4.** Fuzzy, DoS, MITM single e MITM multi, nessa ordem (do que só envia ao que encadeia).

**Consequência para os cenários:** cada ataque tem um modo de entrada, e a rodada precisa respeitá-lo.
DoS: atacante no ar antes da VDE. MITM single: atacante depois da VDE, mais `StopOffer` em nome dela.
MITM multi: .14 antes, .15 depois, `StopOffer` com IP forjado. Fuzzy: a qualquer momento. Um ataque
que entra no meio da rodada e não manda `StopOffer` produz outro efeito (conexão legítima derrubada),
que também é real e pode ser um cenário à parte.

**Critério de aceite, por ataque:** `tools/attack_flows.py` sobre a captura da bancada produz a mesma
assinatura que sobre o PCAP do dataset: mesmos tipos de entrada SD, mesmo lado abrindo o TCP, mesmas
vítimas, proporção UDP/TCP a até 10 pontos percentuais.

## Fase B — Rodada única

**Objetivo:** 9 ECUs + `ids_sensor_node` + atacante na mesma rodada, com janela de ataque
(aquecimento, ataque de `START` a `START+DUR`, recuperação).

**Critério de aceite:** o ataque sai no fio, o PCAP rotulado por IP é gerado, e os security events do
sensor aparecem dentro da janela de ataque.

## Fase C — Medição de detecção, refeita ao vivo

**Objetivo:** recall, precisão e falso positivo por classe sobre as rodadas da Fase B, com o mesmo
`score_pcap` e o controle sobre os PCAPs reais.

**Critério de aceite:** um `DETECCAO.md` novo, substituindo o invalidado, com as rodadas
reproduzíveis por script. Medir também o atraso entre o início do ataque e o primeiro security event.

## Fase D — Controle pela rede `ctrl`

**Objetivo:** ligar, desligar e ajustar cada atacante durante a rodada (taxa, vítima, número de
serviços falsos), com registro de cada comando e seu instante.

**Critério de aceite:** nenhum quadro de controle na captura do barramento; o registro de comandos
permite alinhar o início do ataque com a detecção.

## Fase E — Painel de observação

**Objetivo:** topologia das 9 ECUs com as ligações medidas, tráfego lido do fio, ECU marcada quando
cai numa oferta falsa, security events em tempo de execução, botões da Fase D.

**Critério de aceite:** mostra uma rodada da Fase B do início ao fim.

## Fase F — Editor da matriz

**Objetivo:** modelar nós, serviços, eventos e assinaturas sobre o `comm_matrix.yaml` (o equivalente
ao Communication Setup do CANoe), com o botão "gerar e subir".

**Critério de aceite:** o arquivo editado passa no `tests/check_comm_matrix.py`, gera a configuração
e sobe os nós. Uma matriz diferente da do Kim é marcada como **variação** na rodada.

## Fase G — CARLA

**Objetivo:** conteúdo de condução realista no payload dos provedores (GPS, IMU, VDE), pela rede `ctrl`.

**Critério de aceite:** falso positivo e detecção medidos de novo, com e sem CARLA.

## Fase H — Empacotamento

CLI única (descrever cenário, subir, capturar, rotular, avaliar), `docker-compose`, CI e um exemplo
reproduzível por quem não participou do projeto.

## Opcionais, depois da Fase C

- **Features comportamentais por janela** (taxa de ofertas no SD, IPs de origem distintos, pico de
  taxa), comparadas às de conteúdo: testam a hipótese de que comportamento transfere melhor.
- **SD autenticado como linha de base de defesa:** rodar os ataques com e sem autenticação da oferta
  e medir prevenção e detecção lado a lado. Só com esquema padronizado ou documentado, não um sufixo
  fora do protocolo.

---

## Riscos transversais

| Risco | Mitigação |
|---|---|
| Rótulo por IP marcar ataque com IP forjado como normal | Rótulo pelo MAC; a regra por IP fica só como comparação |
| Atacante fazer algo que a pilha real não permite | A0 testa o comportamento do vsomeip; aceite da Fase A compara com o dataset |
| Tráfego de controle contaminar a captura | Rede `ctrl` separada; aceite da Fase D exige zero quadros de controle no `bus` |
| Interface crescer antes de haver o que mostrar | Painel só depois da Fase B; editor só depois do painel |
| Editar a matriz do Kim e tratar variação como reprodução | O editor marca a rodada como variação |
| Sub-rede do dataset ocupada no host | `SUBNET=` remapeia preservando o último octeto; `attack_flows.py` precisa do conjunto benigno remapeado |

## Pendências de fundo, que não bloqueiam

- 85 dos 108 falsos positivos restantes são Service Discovery (serviço 0xFFFF).
- O `TrainingRunMismatch` segue sem produtor.
- O `run.sh` do sensor não linka o C99 do TL2cgen ao nó; roda com o avaliador compacto.
- Reproduzir o macro-F1 de 0,9689 de ponta a ponta com a cadeia C++ sobre o dataset.

---

## Registro do que já foi concluído

| marco | resultado | onde |
|---|---|---|
| Pilha SOME/IP real no Docker | SD sobe com rota multicast e `NET_ADMIN`; dissector decodifica | `platform/sd-smoke`, `platform/node-handshake` |
| Cadeia do sensor em C++ | paridade bit a bit: 14 375 392 quadros, 0 divergências | `platform/parity-dataset` |
| Artefatos do treino | recuperados e verificados | `docs/ARTEFATOS.md` |
| Sensor no barramento | captura, classifica e reporta ao IdsM | `platform/sensor-live` |
| Reprodução do barramento do Kim | 9 ECUs de um binário; 98% TCP contra 99,1% | `platform/bus-kim` |
| Falso positivo na bancada | 0,0272% no dataset contra 4,11% na bancada (melhor caso); o conteúdo do payload explica 26× (9,42% → 0,3641%) | `experiments/eval-pcap/RESULTADOS.md` |
| Identidade das 9 ECUs | recuperada por conteúdo e ordem dos IPs | `tools/decode_payload_semantics.py` |
| Mecanismo dos ataques | medido nos 4 PCAPs de ataque | `tools/attack_flows.py` |
| Detecção por injeção pós-captura | **invalidada** | `experiments/eval-pcap/DETECCAO.md` |

O detalhamento original de cada marco, escrito na época, segue abaixo sem alterações.

### ✅ Fase 0 concluída

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

### ✅ Caminho crítico da Fase 1 vencido: a cadeia de extração, com paridade

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

### ✅ Artefatos do treino recuperados e verificados (01/11)

Os cinco artefatos foram recuperados da VM de treino. O `Classifier` não roda mais contra um modelo
de prova: roda contra o **modelo adotado** (125 árvores, 25 rodadas, `base_score` em vetor por
classe), e os dois motores reproduzem o `predict` do XGBoost dentro de **2,4e-07** em 2048 linhas
reais do dataset.

As datas dos artefatos **não eram da mesma rodada**, e isso foi verificado em vez de assumido:
`minmax.npy` é **idêntico bit a bit** a um recálculo em float64 a partir dos PCAPs, e
`bytemodels.npz` fica dentro de 1 ULP. Ver [ARTEFATOS](ARTEFATOS.md), que também registra o defeito
de versão do xgboost que quase embarcou um `base_score` errado em silêncio.

### ✅ O sensor está no barramento (01/11)

O `ids_sensor_node` captura, classifica com o modelo adotado e reporta ao IdsM, em
[`platform/sensor-live`](../platform/sensor-live/README.md). Ele entra nos grupos multicast que a
matriz declara, e a lista de eventos de segurança também passou a sair da matriz — trocar um nome
no YAML e esquecer o código faz o `Create` **falhar na subida**, que é o desfecho desejado.

🔴 **E ele produziu o achado que justifica a Fase 1.** Sobre tráfego **benigno**, o sensor levanta
cerca de 30 eventos em 44 pacotes, com confiança mediana de **0,52**. Metade vem de ruído da bridge
que **não é SOME/IP** (o parser aceita qualquer payload UDP de 16+ bytes, fielmente à referência) e
metade são os **nossos próprios anúncios de Service Discovery**, cujos identificadores não estão no
vocabulário em que os modelos de bytes foram ajustados.

**O modelo não transfere para outro barramento sem recalibração.** Não é defeito: no dataset dele o
macro-F1 é 0,9689. É o resultado que só aparece encostando o IDS num barramento de verdade, e é
exatamente o que a Fase 1 existe para resolver — tráfego benigno ancorado no CARLA, gerado *neste*
barramento, para reajustar modelos de bytes e limites.
