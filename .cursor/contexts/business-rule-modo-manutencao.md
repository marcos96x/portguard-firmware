---
alwaysApply: false
---

# Regra de negócio: modo manutenção

Implementada em `script.cpp`. Ativado pela ação `set_maintenance` e desativado pela `unset_maintenance` da fila (`/plaque-queues-actions/list`). `action_data` é ignorado.

## Regras
- Flag `modo_manutencao_ativo` (padrão `false`), persistida na Preferences (chave `manutencao`, namespace `portguard`) junto com intertravamento e fotocélula (`salvaConfiguracaoPortoes()` / `carregaConfiguracaoPortoes()`), para que uma queda de energia durante a manutenção não faça a sirene tocar ao religar.
- Em manutenção:
  - a sirene nunca toca (`calculaSaidas(..., modo_manutencao)` força SAÍDA 7 em LOW e, por ser inversa, SAÍDA 8 em HIGH);
  - abertura sem autorização não arma alarme nem gera `gate_broken`;
  - alarmes que já estavam ativos (abertura indevida, sirene pós-fechamento, alerta de tempo aberto) são encerrados e não voltam quando a manutenção termina;
  - a contagem de tempo aberto fica parada; se o portão continuar aberto ao sair da manutenção, o alerta de 60 s conta a partir da saída;
  - nenhum evento ou log vai para a API: `registraEventoPortao()` descarta os eventos (não entram na fila) e `enviarEventoLog()` descarta tudo, inclusive o heartbeat;
  - a fila continua sendo consultada a cada 500 ms e as ações são executadas normalmente (abrir/fechar portão, intertravamento, fotocélula, Wi-Fi). A resposta das ações (`/plaque-queues-actions/save`) continua sendo enviada.
  - comandos de receptora e intertravamento continuam funcionando.
- Mesmas proteções das outras ações da fila: precisa de `queue_id`, `action_status` diferente de `pending` é ignorado, responde `completed`.

## Estrutura
- `processarAlteracaoFlagPortoes()` trata `set_maintenance`/`unset_maintenance` chamando `defineModoManutencao()`.
- `tarefaControlePortoes()` lê `modo_manutencao_ativo` uma vez por ciclo e passa para `processaPortao(..., modo_manutencao)` e `calculaSaidas(..., modo_manutencao)` (parâmetros com padrão `false`, então os testes antigos não dependem da flag persistida).

## Testes
`teste_modo_manutencao` (em `teste_completo()`): sirene desligada com alarme ativo, abertura sem `gate_broken`, alarmes não voltam após a manutenção, tempo aberto recomeça ao sair, comando funcionando, evento fora da fila da API, `enviarEventoLog` descartando e persistência na Preferences. Restaura o valor original.
