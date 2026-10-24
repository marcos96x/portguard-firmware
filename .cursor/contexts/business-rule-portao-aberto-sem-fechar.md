---
alwaysApply: false
---

# Regra de negócio: portão aberto por mais de 2 minutos (`gate_timeout_open`)

Implementada em `script.cpp`, dentro de `processaPortao()` e `enviarEventoLog()`.

## Regras
- O cronômetro começa quando o sensor abre e zera a cada nova abertura daquele portão. Os portões 1, 2 e 3 contam separados. Vale para abertura por controle, pelo aplicativo e também para abertura sem comando.
- Ao completar `tempo_maximo_portao_aberto_ms` (120 s), a placa registra `gate_timeout_open` uma vez naquela abertura e liga a sirene intermitente (1 s ligada / 2 s desligada) até fechar. Aos 15 s não envia esse alerta.
- O `loop()` envia para `/plaque-events-logs/save` com:
  - `event_type` = `gate_timeout_open`
  - `event_title` = `Portão aberto por mais de 2 minutos`
  - `event_data` = `{"port": N}`, com N = 1, 2 ou 3
- Fechar o portão encerra o alerta e envia `gate_timeout_close` se ele estava ativo. Uma nova abertura pode avisar de novo depois de outros 120 s. Fechar no mesmo instante do limite não avisa.
- Em manutenção a contagem daquele portão fica parada e recomeça ao sair. `down_siren` zera a contagem: o alerta só volta depois de mais 120 s com o portão aberto.
- A comparação do tempo usa subtração de `millis()` e continua válida no estouro de ~49 dias.
- A sirene contínua de abertura sem autorização continua independente deste alerta.

## Testes
`teste_aviso_portao_aberto_sem_fechar` (em `teste_completo()`): título e `event_data`, nenhum aviso aos 15 s, aviso único aos 120 s, nova abertura zera a contagem, fechamento no limite não avisa, portões separados, manutenção pausa o cronômetro e o estouro do `millis()` continua correto. Não faz requisição HTTP.
`teste_portao_aberto_tempo_excessivo` continua conferindo a sirene intermitente e o `gate_timeout_close`.
