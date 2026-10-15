# someip-testbench

Bancada **aberta e reprodutível** de simulação, injeção de ataques e captura de tráfego
**SOME/IP**, ancorada na dinâmica de um veículo simulado no **CARLA**, com nós de comunicação
**em C++** seguindo o padrão da **AUTOSAR Adaptive Platform** e saída de **Security Events**
para um **IdsM**.

## Por que existe

Os datasets SOME/IP mais realistas da literatura são construídos com cadeias **proprietárias**
(PreScan + Simulink + **Vector CANoe**), o que limita a reprodutibilidade. Os geradores abertos,
por outro lado, produzem tráfego sintético, sem dinâmica de veículo nem semântica de sensores.

Esta bancada ocupa esse meio: entrega o **núcleo funcional de um CANoe para SOME/IP**
(simulação de nós ECU a partir de uma matriz de comunicação, trace/captura, injeção de falhas)
usando apenas software aberto, e acrescenta uma camada que o CANoe **não tem**: a cadeia
**Security Event → IdsM** da AUTOSAR Adaptive.

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

## Repos relacionados

| Repo | Papel |
|---|---|
| `someip-ids-multiclass-contentext` | IDS multiclasse (XGBoost + content_ext) — vira o `ids_sensor` |
| `someip-traffic-simulator` | Gerador YAML e taxonomia de ataques — origem dos cenários |
| `carla-someip` | Protótipo anterior (Python, CAN-style) — referência de sensores/ataques |
| `carla-autoware-someip-ids` | Visão maior (malha fechada com Autoware) — consumirá esta bancada |

## Status

Em projeto. Nenhum código ainda: esta é a fase de arquitetura.
