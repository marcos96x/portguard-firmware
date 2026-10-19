---
alwaysApply: false
---

# Regra de negócio: limite de envio do gate_broken

Implementada em `script.cpp`, dentro de `enviarEventoLog()`.

## Regras
- `gate_broken` é notificação crítica. Ao ser enviado com sucesso para a API, o momento (`millis()`) é salvo em `momento_ultimo_gate_broken_enviado` e `gate_broken_ja_enviado` passa a `true`.
- Antes de enviar um `gate_broken`, verifica `gateBrokenDentroDoIntervaloMinimo()`: se o último envio foi há menos de `intervalo_minimo_entre_gate_broken_ms` (60 s), o evento é descartado (não vai para a API) e registra log.
- O limite é único para a placa (vale para os três portões juntos).
- Só envio com sucesso conta: se o POST falhar, o próximo `gate_broken` é enviado normalmente.
- Apenas o evento enviado para a API é limitado: sirene, alarme e log local continuam iguais.
- Em modo manutenção nenhum evento é enviado (a verificação de manutenção vem antes).
- Comparação segura contra o estouro do `millis()` (~49 dias).

## Testes
`teste_limite_gate_broken` (em `teste_completo()`): sem envio anterior não bloqueia, bloqueia até 59,999 s, libera aos 60 s e funciona no estouro do `millis()`. Testa só a decisão, sem requisição HTTP.
