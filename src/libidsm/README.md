# `libidsm` — Security Event e IdsM

Fase 3 do [plano](../../docs/PLANO.md). É o elo que o artigo deixa como trabalho
futuro: a classe predita pelo detector deixa de ser um número e passa a ser um
**Security Event tipado**, consumido por um Intrusion Detection System Manager.

## O que vai aqui

| Peça | Papel |
|---|---|
| `EventReporter` | o que um nó chama para reportar: `ReportEvent(SecurityEventDefinition, contexto)` |
| `IdsM` | recebe os eventos e aplica a cadeia de filtros |
| cadeia de filtros | limite de taxa, agregação por janela, limiar de severidade |
| reporter de saída | log estruturado e, opcionalmente, um serviço SOME/IP representando o SOC |

A cadeia de filtros não é detalhe de implementação: é o que distingue um IdsM de
um arquivo de log. Sem ela, um DoS gera um evento por pacote e afoga o consumidor.

## Por que a classe não é o evento

O detector produz uma de cinco classes. O IdsM espera um
`SecurityEventDefinition`, com identificador estável, severidade e contexto
(`SecurityEventContextMapping`). O mapeamento entre os dois é uma decisão de
projeto, não uma conversão: duas classes podem virar o mesmo evento com contextos
diferentes, e uma classe pode não gerar evento algum abaixo de um limiar.

## Referências

- Spec do IdsM da AUTOSAR Adaptive: `EventReporter`, `SecurityEventDefinition`,
  `SecurityEventContextMapping`. No Classic, `IdsM_ReportEvent`.
- `Jonas0828/autosar-idsm` no GitHub: implementação aberta, útil como referência
  antes de escrevermos esta.

## Critério de pronto

Um cenário de ataque produz um **log de eventos qualificados** alinhado
temporalmente com a janela do ataque injetado, não um evento por pacote.
