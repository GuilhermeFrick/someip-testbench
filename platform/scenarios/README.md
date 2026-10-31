# `scenarios` — cenários de ataque, em YAML

Fase 2 do [plano](../../docs/PLANO.md). Um cenário descreve **o que acontece e
quando**, e é lido tanto pelo [`attacker`](../../src/attacker/) quanto pelo
rotulador.

É essa leitura dupla que faz o ground truth ser exato: o cenário é a timeline, e
o `tools/label_pcap.py` rotula a partir dela mais o mapa de IP para papel. Não há
rotulagem por heurística em cima do PCAP.

## O que um cenário declara

- qual ataque, da taxonomia herdada do `someip-traffic-simulator`
- janela de início e duração
- `trigger_rate` e o jitter, que é o que evita o viés de padrão perfeito
- qual nó é a origem e qual é o alvo

## Relação com a matriz

Os nós e serviços citados num cenário precisam existir em
[`comm_matrix.yaml`](../comm_matrix.yaml). A matriz é a fonte única: cenário que
cita um serviço inexistente é erro de validação, não comportamento emergente.
