# `scenario` — a camada CARLA

Fase 1 do [plano](../../docs/PLANO.md). É a única parte da bancada em **Python**,
por decisão travada: o resto dos nós é C++.

## `carla_feeder.py`

Roda o CARLA em **modo síncrono** e alimenta os sensores dos
[`nodes`](../src/nodes/) pela rede `ctrl`.

O modo síncrono não é preferência, é requisito: ele dá um **clock único** ao
experimento. É esse clock que permite carimbar simulação, injeção de ataque,
captura e detecção na mesma base de tempo, o que é o risco transversal de
alinhamento temporal listado no plano.

## Duas redes, e por quê

| Rede | Tráfego | Capturada |
|---|---|---|
| `ctrl` | CARLA alimentando os sensores | não |
| `bus` | SOME/IP entre as ECUs | **sim** |

A separação existe para o tráfego de controle não contaminar o dataset. A captura
é restrita à `bus`.

## Por que a física importa

O objetivo da bancada não é gerar volume de pacotes, é gerar tráfego cujo
**padrão temporal tenha causa**. Um sensor alimentado por condução real acelera,
freia e curva, e isso aparece na série temporal que o detector consome. Tráfego
cíclico puro não exercita as features de intervalo.
