---
alwaysApply: false
---

# Regra de negócio: janela para o sensor abrir depois do comando

Implementada em `script.cpp`. Vale para o comando na receptora da placa e para `open_or_close_gate` do aplicativo.

## Regras
- Com o portão fechado, o comando libera **13 segundos** para o sensor daquele portão abrir (`janela_comando_autorizado_ms` = 8 s originais + 5 s).
- Abertura dentro desses 13 s é autorizada: não gera `gate_broken` e não liga a sirene.
- Se o sensor desse portão abrir depois dos 13 s, é arrombamento: sirene contínua e `gate_broken`.
- O outro portão não entra nessa janela. Abrir sem comando continua sendo arrombamento na hora.
- Se o sensor do portão comandado não abrir e permanecer fechado, a autorização expira sem sirene e conta como possível falha de sensor (ver `business-rule-funcionamento-basico.md`).

## Testes
`teste_janela_abertura_comando_estendida` (em `teste_completo()`): abertura aos 8 s ainda é autorizada; abertura depois de 13 s liga a sirene só no portão que recebeu o comando.
