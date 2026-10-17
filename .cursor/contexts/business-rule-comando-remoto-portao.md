---
alwaysApply: false
---

# Regra de negócio: abrir/fechar portão via API (`open_or_close_gate`)

A consulta periódica da fila (`buscarConfigWifiNaApi()` via `/plaque-queues-actions/list`, a cada 500 ms com rede — ver `business-rule-configuracao-remota-placa.md`) também trata a ação `open_or_close_gate`. Implementada em `script.cpp`.

## Regras
- Portão alvo vem de `action_data` (`extraiNumeroPortaoDoActionData()`):
  - ausente, vazio, `null` ou `1` = portão 1;
  - `2` = portão 2;
  - outro valor = inválido, responde `failed` sem acionar nada.
  - Aceita número (`2`), texto (`"2"`) ou JSON normal/escapado (`{"port":2}`).
- O comando tem o mesmo efeito de um acionamento da receptora do portão:
  - portão fechado: pulso no motor e janela de autorização (a abertura não gera `gate_broken`);
  - portão aberto: pulso no motor para fechamento;
  - respeita o intertravamento (`business-rule-intertravamento.md`): abertura com o outro portão aberto é bloqueada.
- Resposta à fila (`/plaque-queues-actions/save`): `completed` se o motor foi acionado; `failed` se bloqueado pelo intertravamento, `action_data` inválido ou tarefa dos portões sem resposta.
- O `queue_id` é marcado em `ultimoQueueIdNotificado` antes de acionar o portão: mesmo que a resposta à API falhe, o mesmo comando nunca é executado duas vezes. Sem `queue_id` o comando não é executado.
- O `id` da fila é lido como número (`"id":12`) ou texto (`"id":"12"`).
- Ação com `action_status` diferente de `pending` é ignorada (evita repetir comando antigo após reiniciar a placa).
- Comando criado há mais de 60 s (`created_at` x relógio NTP) não é executado e responde `failed` (ver "Ações imediatas antigas" em `business-rule-configuracao-remota-placa.md`).

## Estrutura
- `processarAbrirOuFecharPortao(queue_id, payload)` roda no `loop()` (rede). `enviaComandoRemotoParaTarefaPortoes()` envia o número do portão para `fila_comandos_remotos_portao` e espera até `tempo_espera_resultado_comando_remoto_ms` (500 ms) o resultado em `fila_resultados_comandos_remotos` (a mesma função entrega o `down_siren`, código `comando_remoto_desligar_sirene` = 3).
- `tarefaControlePortoes()` consome o comando a cada ciclo e passa `comando_remoto = true` para `processaPortao()` do portão alvo; devolve o retorno de `processaPortao()` (false = bloqueado pelo intertravamento).
- O portão nunca é acionado diretamente pelo `loop()`, para não haver concorrência com a tarefa dos portões.

## Testes
`teste_comando_remoto_portao` (chamado em `teste_completo()`): formatos de `action_data`, abertura remota autorizada sem `gate_broken`, bloqueio pelo intertravamento e fechamento remoto de portão aberto.
