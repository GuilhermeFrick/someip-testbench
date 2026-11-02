# Tráfego adversarial para avaliação de detecção

⚠️ **O que está IMPLEMENTADO é a injeção pós-captura** (`tools/inject_scenarios.py`),
não um nó vivo. A razão está em `experiments/eval-pcap/DETECCAO.md`: é o mesmo
método dos dois baselines (Luo injeta por script CAPL; Kim publica PCAPs rotulados),
é verificável sem subir o barramento, e roda sobre o substrato que importa — a
reprodução em vsomeip real do barramento validado.

As quatro famílias (DoS, Fuzzy, MITM de uma origem, MITM de várias) são reproduzidas
com assinatura **medida do dataset real**: serviço, transporte, tipo, cadência e
densidade de ataque. Isso corrigiu nosso próprio gerador de apoio
(`someip-traffic-simulator`), que usava relay `0x100B` para todo MITM — no dataset
só o *multi* usa relay; o *single* impersona o serviço legítimo.

## Ground truth

Por **IP de origem**: o injetor usa um endereço fora do conjunto benigno, e toda
frame dele leva a classe do cenário. É a regra do `extract.py` de referência
(`src ∉ benign_ips`), a mesma que rotula o dataset. Sem heurística sobre o PCAP.

## O resultado, em uma linha

Só o Fuzzy transfere para a bancada (recall 0,59); DoS e MITM colapsam (0–3%),
contra 0,77–0,99 no dataset real. O detector detecta por conteúdo específico do
domínio, não por estrutura de ataque. Ver `experiments/eval-pcap/DETECCAO.md`.

## O nó vivo (superfície futura)

DoS e Fuzzy podem rodar como nó de socket cru (não precisam observar o barramento
antes); MITM precisa observar-e-retransmitir. Fica como trabalho futuro — a injeção
pós-captura já dá o número de detecção, que é o que faltava.
