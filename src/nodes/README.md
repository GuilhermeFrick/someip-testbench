# `nodes` — os nós ECU benignos

Fase 1 do [plano](../../docs/PLANO.md). Cada nó é um executável C++ sobre a
[`libecu`](../libecu/), e existe para produzir tráfego SOME/IP que **acompanhe a
dinâmica de um veículo**, em vez de um padrão temporal sintético.

| Nó | Papel | Publica |
|---|---|---|
| `gnss_ecu` | sensor | posição, velocidade, status |
| `imu_ecu` | sensor | aceleração, taxa angular |
| `lidar_ecu` | sensor | obstáculo mais próximo (nuvem reduzida) |
| `adas_ecu` | função | estado de AEB, derivado dos três sensores |
| `hmi_ecu` | consumidor | assina e exibe |

O `adas_ecu` é o único que **consome e publica**, e é ele que justifica a bancada
ter `Method` além de `Event`: a função derivada precisa de request/response, não
só de pub/sub. Sem ele, o dataset teria apenas notificação cíclica.

## De onde vem a física

Do [`scenario/carla_feeder.py`](../../scenario/), pela rede `ctrl`, separada da
rede `bus` que é capturada. A separação é requisito: tráfego de controle na rede
capturada contaminaria o dataset.

## Risco conhecido

O volume do LiDAR. Mitigação já decidida: publicar apenas o obstáculo mais
próximo, que é o que o `adas_ecu` de fato consome.

## Critério de pronto

Um PCAP benigno de alguns minutos, com pub/sub e request/response, em que o
padrão temporal do tráfego acompanha a condução: acelerações, curvas, frenagens.
