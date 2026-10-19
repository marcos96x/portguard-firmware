---
alwaysApply: false
---

# Regra de negócio: modo manutenção

Implementada em `script.cpp`. A fila envia `maintenance_port_1`, `maintenance_port_2` ou `maintenance_port_3` com `action_data` `"1"` (entra) ou `"0"` (sai). `set_maintenance` e `unset_maintenance` aplicam o mesmo estado a todos os portões que a placa tem.

## Regras
- Uma flag por portão: `manutencao_portao_1`, `manutencao_portao_2`, `manutencao_portao_3` (padrão `false`). Persistidas em `manutencao_1`, `manutencao_2` e `manutencao_3` (namespace `portguard`). A chave antiga `manutencao` ainda serve de padrão só para o portão 1.
- Só o portão indicado muda. Os outros continuam com sirene, alarme e eventos.
- No portão em manutenção:
  - abertura sem autorização não arma alarme nem gera `gate_broken`;
  - alarmes já ativos daquele portão são encerrados e não voltam quando a manutenção termina;
  - a contagem de tempo aberto fica parada e recomeça ao sair, se o portão continuar aberto;
  - evento daquele portão não entra na fila e `enviarEventoLog` descarta. Heartbeat e eventos dos outros portões seguem;
  - o intertravamento ignora esse portão (nem bloqueia os outros, nem é bloqueado);
  - comando de receptora continua funcionando.
- Portão que a placa não tem (ver `business-rule-quantidade-portoes.md`) recusa a ação com `failed`.
- A fila continua sendo consultada. Precisa de `queue_id`. `action_status` diferente de `pending` é ignorado. Responde `completed`.

## Estrutura
- `defineManutencaoPortao(numero_portao, ativo)` grava a flag do portão.
- `processarAlteracaoFlagPortoes()` trata `maintenance_port_N` e o par `set_maintenance`/`unset_maintenance`.
- `tarefaControlePortoes()` passa `portaoEmManutencao(N)` para `processaPortao` daquele portão.

## Testes
`teste_modo_manutencao` (em `teste_completo()`): sirene desligada no portão em manutenção, abertura sem `gate_broken`, alarmes não voltam, tempo aberto recomeça, comando funcionando, evento do portão 1 fora da fila e evento do portão 2 ainda entra, persistência de `manutencao_1`. Restaura as três flags.
