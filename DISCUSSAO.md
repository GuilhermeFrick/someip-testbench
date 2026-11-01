# Discussão: o que medimos, o que descobrimos e o que muda

> Documento de memória das decisões de rumo. Não é documentação da bancada — para
> isso há o [README](README.md), o [PLANO](docs/PLANO.md) e os degraus em
> [`platform/`](platform/). Aqui ficam as **discussões**, com os números que as
> sustentam e as perguntas que seguem abertas.
>
> Sessão de **01/11/2026**.

---

## 1. As hipóteses que o artigo assume

1. **Sem identificadores de cabeçalho generaliza melhor.** Usar *Service/Method ID*
   como feature prenderia o modelo a valores de identificador, não a comportamento,
   "o que compromete a generalização para outras implementações e para ataques sobre
   serviços não vistos no treinamento" (§II).
2. **As 4 features comportamentais separam os tipos de ataque.** O baseline do Kim é
   binário; a C1 afirma que conteúdo + comportamento bastam para distinguir 5 classes.
3. **Multiclasse é acionável, binário não.** Sinal tipado alimenta o IdsM da AUTOSAR
   Adaptive.
4. **É leve o bastante para ser sensor de runtime.** 16 features por pacote, com
   latência medida.
5. **(C2) O framework mede generalização com o pipeline CONGELADO.** Modelo de bytes,
   normalização e XGBoost do Kim aplicados a tráfego de outra origem, sem retreinar.

---

## 2. O que foi medido hoje

### 2.1 A cadeia em C++ reproduz o Python, exatamente

| o quê | resultado |
|---|---|
| extração das 16 features, 7 PCAPs | **14 233 354 vetores, 0 divergências**, bit a bit |
| avaliador compacto × `predict` do XGBoost | **2,38e-07**, classe igual em 2048/2048 |
| C99 do TL2cgen × `predict` do XGBoost | **2,09e-07**, classe igual em 2048/2048 |

A soma dos vetores por arquivo dá exatamente os **14 233 354** que a tabela do artigo
reporta.

### 2.2 O sensor no barramento, com tráfego benigno da bancada

```
pacotes aceitos: 43     descartados pelo parser: 80     eventos de segurança: 28
confiança: min 0,51   mediana 0,52   max 0,93
```

| origem | eventos | serviço | o que é |
|---|---:|---|---|
| `172.30.0.11` (nosso provedor) | 15 | `0xFFFF` | **os nossos próprios `OfferService`** |
| `172.30.0.1` (gateway Docker) | 13 | `0x0000` | tráfego que **não é SOME/IP** |

### 2.3 O dataset do Kim, aberto

| fato | valor |
|---|---|
| infraestrutura | **Docker + vsomeip** (MAC `02:42:ac:12:...`, rede `172.18.0.0/16`, SD multicast `224.244.224.245` — o padrão do vsomeip) |
| nós | **9** (`172.18.0.2` … `.10`) |
| transporte | **99,1% TCP**, UDP quase só para SD |
| pacotes sem payload | **~50%** são ACKs de TCP |
| segmentos TCP com payload | **247 880, todos com UMA mensagem SOME/IP completa** |
| descartes do parser | **0,04%** (contra 65% na nossa bancada) |

---

## 3. As descobertas, por ordem de importância

### 3.1 🔴 O dataset do Kim é a MESMA arquitetura da nossa bancada

Docker + vsomeip, mesmo endereço multicast de SD. Isso tem duas consequências opostas,
e as duas importam:

- **A favor:** a distância entre os domínios é muito menor do que parecia. Reproduzir
  o barramento dele na nossa bancada é viável, e vira **condição de controle**.
- **Contra:** o posicionamento da C2 como "framework aberto de geração de tráfego
  SOME/IP rotulado" é **menos novo do que o texto sugere**. ❓ *Precisa verificar o que
  o Kim publicou: só os PCAPs, ou o gerador também?* Se publicou o gerador, os nossos
  diferenciais têm de ser reafirmados com precisão: payload vindo da física do CARLA,
  a matriz como fonte única, a cadeia IdsM, e a capacidade de **medir** transferência.

### 3.2 🔴 O modelo não transfere, e as causas são identificáveis

Sobre tráfego benigno da bancada, o detector congelado dispara em ~70% dos pacotes que
aceita, com confiança mediana de 0,52 — em cinco classes, isso é pouco acima do nada.
**O modelo está extrapolando, não detectando.**

Duas causas distintas:

1. **Metade nem é tráfego do barramento.** O parser aceita **qualquer payload UDP de 16+
   bytes** como cabeçalho SOME/IP, fielmente à referência. É a mesma família do *padding*
   Ethernet achado na paridade. Num dataset limpo isso nunca aparece; num segmento real,
   com ruído de infraestrutura, **vira detecção**.
2. **A outra metade são os nossos próprios anúncios de serviço.** O `service 0x1234` não
   está no vocabulário em que os modelos de bytes foram ajustados.

**Isto não desabona o IDS** — no dataset dele o macro-F1 é 0,9689. E **o artigo já previa
o mecanismo** (§V-C: *"um benigno com distribuição diferente pode elevar falsos positivos;
medir esse desvio de domínio é parte do resultado"*).

### 3.3 🟡 Tensão fina com a hipótese 1

O artigo é agnóstico a cabeçalho **no nível das features** — nenhuma feature é o
*Service ID*. Mas o `pl_sd` **contém** os identificadores nos bytes das entradas de SD, e
o modelo de bytes pontua esses bytes. **A sensibilidade a identificador volta pela porta
dos fundos**, via verossimilhança e entropia do payload de SD.

Evidência: 15 dos 28 eventos foram exatamente `OfferService` do nosso provedor.

❓ *A decidir: isto entra na discussão do artigo como limitação, ou é melhor medir
primeiro com a bancada reproduzindo o barramento do Kim (onde os IDs coincidem) para
isolar o efeito?*

### 3.4 🟡 A diferença de domínio é bem maior que identificador

**99% TCP × 100% UDP**, e **metade do treino são ACKs com payload vazio** — pacotes cujas
seis features de conteúdo valem **zero por definição**, e que a nossa bancada nunca
produz. Atribuir o falso positivo ao vocabulário de serviço antes de a bancada falar TCP
seria conclusão apressada.

### 3.5 🟢 A frase da remontagem TCP no artigo está errada, e agora dá para corrigir com dado

§IV diz que os segmentos TCP "são reordenados por número de sequência e remontados". O
parser não faz isso — e **não precisa**: 247 880 de 247 880 segmentos com payload trazem
uma mensagem completa. Resolve o `\cgf` do orientador com medição, não com opinião.

### 3.6 🟢 "Barramento puro" é propriedade do dataset, não da implantação

O próprio modelo de ameaça do artigo põe o IDS numa **porta espelhada de switch** — e uma
porta espelhada real vê ARP, housekeeping, diagnóstico. Os 99,96% de limpeza do Kim podem
estar deixando o problema mais fácil do que ele é no veículo.

---

## 4. As decisões tomadas

### 4.1 Construir sobre uma base validada pela academia

Em vez de comparar a bancada do zero contra o dataset, **reproduzir o barramento do Kim** e
só então variar uma coisa por vez. Hoje temos *um ponto* ("mudou tudo e disparou"), sem
atribuição de causa. Com a reprodução temos uma escada:

```
barramento do Kim reproduzido   → falso positivo esperado ≈ o do próprio dataset
  + conteúdo do payload (CARLA) → mede o efeito da física
  + identificadores diferentes  → mede o efeito do vocabulário
  + número de ECUs              → mede o efeito da topologia
  + transporte UDP              → mede o efeito do transporte
```

É a mesma lógica que validou o gerador na Fase 0, quando os identificadores da nossa
matriz coincidiam de propósito com os dos exemplos do vsomeip.

**Feito hoje:** `tools/extract_matrix.py` recupera a matriz de comunicação de uma captura.
Rodado sobre o benigno do Kim, recuperou:

| serviço | provedor | endpoints | eventos | eventgroups |
|---|---|---|---:|---:|
| `0x1001` | `172.18.0.10` | tcp:30501 udp:31097 | 4 | 4 |
| `0x1002` | `172.18.0.5` | tcp:30502 udp:31098 | 4 | 4 |
| `0x1003` | `172.18.0.4` | tcp:30503 udp:31099 | 3 | 3 |

Cadências **5, 10, 15 e 25 ms**; payloads de **16, 24, 32 e 40 bytes**; 6 consumidores.
As portas TCP recuperadas batem com as medidas no tráfego por outro caminho.

### 4.2 Calibração por domínio, classificador congelado

| camada | precisa de quê | onde treina |
|---|---|---|
| features (as 16) | nada, é código | fixo |
| modelo de bytes + limites | só benigno, **sem rótulo** | **no barramento alvo** |
| XGBoost | ataques rotulados | fábrica, uma vez, toda a frota |

A granularidade não é por carro, é **por matriz de comunicação** — carros do mesmo
variante têm o mesmo tráfego. E a bancada gera tráfego **a partir de uma matriz**, então
ela é a ferramenta que produz o tráfego de calibração de uma matriz nova.

A tese da C2 passa a ser: **classificador congelado em frota, perfil de conteúdo calibrado
por domínio, sem rótulos.** É mais defensável, e é a forma que um produto teria de ter.

❓ *A decidir: os limites min-max do treino foram calculados sobre benigno **+ ataque**.
Num carro só há benigno. Recalcular só no benigno muda a escala — e as árvores aprenderam
limiares na escala antiga. É a primeira coisa que o experimento tem de medir.*

### 4.3 Filtro de captura: sim, mas não por porta

Filtrar é **decisão de segurança**, não limpeza. Filtrar por porta cegaria o detector para
tráfego fora da matriz — que num barramento automotivo é, por definição, anômalo.
Preferir: filtrar pela **sub-rede do barramento**, ou manter o filtro largo e **contar** o
tráfego fora da matriz como sinal próprio.

E "fora da matriz é evidência" **não entra nas 16 features** — isso contradiria a hipótese
1 de frente. Entra como **um segundo sensor**, baseado em identificador, que reporta o seu
próprio *Security Event*. O IdsM existe exatamente para agregar sensores de tipos
diferentes.

---

## 5. O que muda no artigo

| onde | o quê |
|---|---|
| §IV, remontagem TCP | corrigir: o parser trabalha por pacote, e a medição mostra que basta |
| §V, dataset | dizer que é **99% TCP** e que **~50% são ACKs** com features de conteúdo nulas |
| §V-C, transferência | nomear as diferenças reais: transporte, nº de ECUs, identificadores |
| C2, posicionamento | o dataset do Kim também é Docker+vsomeip; reafirmar os diferenciais com precisão |
| discussão | a dependência de identificador que volta pelo `pl_sd` |

---

## 6. Perguntas abertas

1. O Kim publicou o **gerador**, ou só os PCAPs?
2. Recalcular min-max só no benigno ajuda ou atrapalha?
3. Depois de recalibrar, o falso positivo cai ao nível do dataset? **Se não cair, a
   conclusão honesta é que features de conteúdo por modelo de bytes não transferem entre
   implantações SOME/IP** — ainda publicável, mas resultado negativo para o método, e o
   produto passaria a exigir treino por barramento.
4. A bancada deve igualar as 9 ECUs do Kim, ou 5 bastam?

---

## 7. Próximos passos

1. **Gerar a bancada a partir da `comm_matrix_kim.yaml`** — exige estender o
   `gen_config.py` e a `libecu` para **TCP** e para múltiplos endpoints.
2. **Rodar o pipeline congelado sobre o tráfego reproduzido.** É o teste que valida a
   bancada: se o falso positivo cair ao nível do dataset, a escada da §4.1 fica em pé.
3. **Reproduzir o macro-F1 (0,9689)** com a cadeia em C++ sobre o dataset, fechando a
   verificação ponta a ponta — hoje as peças estão verificadas, o número final não.
4. Subir a escada, um degrau por vez, medindo falso positivo em cada um.
