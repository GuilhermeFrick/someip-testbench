# someip-testbench

Bancada de simulação, injeção de ataques e captura de tráfego **SOME/IP**, ancorada na dinâmica de
um veículo simulado no **CARLA**, com nós de comunicação **em C++** seguindo o padrão da
**AUTOSAR Adaptive Platform** e saída de **Security Events** para um **IdsM**.

Repositório de trabalho de mestrado, aberto por enquanto.

## Por que existe

Os datasets SOME/IP mais realistas da literatura são construídos com cadeias proprietárias
(PreScan + Simulink + Vector CANoe). Os geradores abertos, por outro lado, produzem tráfego
sintético, sem dinâmica de veículo nem semântica de sensores.

Esta bancada fica no meio: simula nós ECU a partir de uma matriz de comunicação, captura o
barramento e injeta falhas, como o núcleo funcional de um CANoe para SOME/IP, e acrescenta uma
camada que o CANoe não tem: a cadeia **Security Event → IdsM** da AUTOSAR Adaptive.

## O que ela faz

1. **Simula nós ECU** (*rest-bus simulation*) em C++ sobre a pilha **vsomeip**, com SOME/IP-SD real.
2. **Ancora o tráfego na física**: o CARLA alimenta os serviços com GNSS, IMU, LiDAR e dinâmica.
3. **Injeta ataques** rotulados (DoS, Fuzzy, MITM) a partir de cenários em YAML.
4. **Captura** o barramento em PCAP com *ground truth* por pacote.
5. **Reporta Security Events**: o IDS atua como sensor e reporta ao IdsM, que filtra e qualifica.

## Equivalência com o Vector CANoe

| Vector CANoe | Aqui |
|---|---|
| Database (ARXML/FIBEX) | Matriz de comunicação (YAML) → gera config vsomeip + código C++ |
| Rest-bus simulation | Nós ECU em C++ sobre vsomeip (Skeleton/Proxy) |
| CAPL | Lógica C++ do nó + cenário YAML |
| Trace window | PCAP + dissector SOME/IP do Wireshark + rotulador próprio |
| Fault injection | `attacker_ecu` (via serviço) + injeção *on-the-wire* |
| Panels (estímulo manual) | **CARLA** (estímulo físico real) |
| Interface de hardware (VN) | Rede virtual (Docker bridge) |
| — | **IdsM + Security Event + sensor de ML** |

**Não** pretende igualar: GUI interativa, CAPL, hardware real, nem a amplitude de protocolos.

## Documentação

- [`docs/ARQUITETURA.md`](docs/ARQUITETURA.md) — camadas, diagramas, decisões técnicas
- [`docs/PLANO.md`](docs/PLANO.md) — fases, entregáveis e riscos
- [`docs/ARTEFATOS.md`](docs/ARTEFATOS.md) — os três artefatos do treino, a conversão e a verificação
- [`tests/README.md`](tests/README.md) — a suíte, o que cada teste pega e o que ainda falta
- [`platform/sd-smoke`](platform/sd-smoke/README.md) — o Service Discovery entre containers
- [`platform/node-handshake`](platform/node-handshake/README.md) — dois nós nossos trocando SOME/IP
- [`platform/parity-dataset`](platform/parity-dataset/README.md) — a extração em C++ contra o Python,
  sobre os 14,4 M pacotes do dataset

## Repos relacionados

| Repo | Papel |
|---|---|
| `someip-ids-multiclass-contentext` | IDS multiclasse (XGBoost + content_ext) — vira o `ids_sensor` |
| `someip-traffic-simulator` | Gerador YAML e taxonomia de ataques — origem dos cenários |
| `carla-someip` | Protótipo anterior (Python, CAN-style) — referência de sensores/ataques |
| `carla-autoware-someip-ids` | Visão maior (malha fechada com Autoware) — consumirá esta bancada |

## Status

**Fase 0 fechada** e o caminho crítico da **Fase 1** vencido.

| | Estado |
|---|---|
| Matriz de comunicação → config vsomeip + cabeçalhos C++ | feito ([`tools/gen_config.py`](tools/gen_config.py)) |
| SOME/IP-SD entre containers | validado |
| `libecu` (`Skeleton`/`Proxy`) e dois nós ECU trocando evento | **passou**, decodificado pelo dissector |
| Sensor: fonte de pacotes, parser, modelo de bytes e extrator das 16 features | feito, com **paridade exata** contra o Python sobre o dataset inteiro |
| Normalização, dois motores de inferência, despacho de Security Event, cadeia completa | feito, **352 asserções** passando |
| Artefatos do treino (modelo, modelos de bytes, limites) | recuperados e **verificados**: `minmax` idêntico bit a bit ao recálculo, `bytemodels` dentro de 1 ULP |
| Nó de sensor no barramento; CARLA alimentando os serviços; injeção de ataques | Fases 1 e 2 |

O número que sustenta a cadeia de extração: **14 375 392 quadros, 14 233 354 vetores de features,
0 divergências** contra o código Python que treinou o modelo, exato bit a bit.

Os dois motores de inferência conferidos contra o `predict` do próprio XGBoost **com o modelo
adotado**, em 2048 linhas reais do dataset: pior diferença de **2,38e-07** (tabelas) e **2,09e-07**
(C99 do TL2cgen), com a classe escolhida batendo em todas.
