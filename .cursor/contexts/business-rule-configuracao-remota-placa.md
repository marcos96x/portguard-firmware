---
alwaysApply: false
---

# Regra de negócio: configuração remota da placa (intertravamento, fotocélula) e consulta rápida da fila

Implementada em `script.cpp`.

## Consulta da fila a cada 500 ms
- `verificarRede()` chama `buscarConfigWifiNaApi()` (`/plaque-queues-actions/list`) a cada `intervalo_consulta_fila_ms` (500 ms) quando há Wi-Fi ou cabo conectado.
- Após uma falha da consulta espera `intervalo_espera_apos_falha_consulta_fila_ms` (5 s) antes de tentar de novo.
- Sem rede, a consulta continua sendo tentada junto com o heartbeat (`INTERVALO_API_MS`, 30 s). O heartbeat não mudou.
- Cada consulta faz um handshake HTTPS; na prática o intervalo real é 500 ms ou o tempo da requisição, o que for maior. O controle dos portões roda em tarefa própria e não é afetado.
- Logs: a consulta só registra erros (`requisicao_sem_log_de_sucesso`); sem ação nova nada é impresso; os dados de uma ação aparecem só na primeira leitura (`ultimo_queue_id_exibido_no_log`).
- `config_plaque` com conexão Wi-Fi em andamento (`wifiCredenciaisPendentes` e mesmo `queueIdPendente`) é ignorado nas consultas seguintes para não reiniciar a conexão a cada 500 ms.
## A fila nunca pode travar
- A API (`limit=1`) devolve sempre a ação **pendente mais antiga**. Uma ação que a placa lê e não responde fica na frente de todas as outras para sempre (foi o que travou `open_or_close_gate`, `intertravamento` e fotocélulas atrás de um `down_siren` não tratado).
- Por isso toda ação lida é respondida em `/plaque-queues-actions/save`:
  - ação desconhecida: `failed` (log "Acao desconhecida ignorada");
  - `config_plaque` sem SSID: `failed`;
  - `config_plaque` igual à persistida: `completed`, mesmo com o Wi-Fi fora (antes só respondia com Wi-Fi conectado).
- `atualizarStatusFila()` guarda o último status respondido (`queue_id_ultimo_status_respondido`, `ultimo_status_respondido_fila`). Se a API continuar devolvendo uma ação já tratada (ex.: falha de rede no `/save`), o status é reenviado a cada `intervalo_reenvio_status_fila_ms` (5 s) **sem executar a ação de novo**.

## Ações imediatas antigas (`open_or_close_gate`, `down_siren`)
- `acaoDaFilaExpirada(payload, agora_epoch)`: ação criada (`created_at`, horário de Brasília, `fuso_horario_servidor_segundos` = -3 h) há mais de `validade_acao_imediata_fila_segundos` (60 s) não é executada e responde `failed`. Evita abrir portão ou calar a sirene com comandos acumulados (placa desligada ou fila travada).
- O relógio vem do NTP (`configTime` em UTC, iniciado em `verificarRede()` na primeira vez que há rede). Ação imediata lida antes da sincronização espera até `tempo_maximo_espera_sincronizacao_relogio_ms` (15 s) sem ser respondida; depois disso, sem relógio, sem `created_at` ou com `created_at` no futuro a ação é executada normalmente.
- Ações de configuração (intertravamento, fotocélulas, manutenção) não expiram: são aplicadas em ordem e o estado final é o da última ação.

## Ações `intertravamento` e fotocélulas
- `action_data` (`extraiFlagDoActionData()`): `0`/`false` = desligar, `1`/`true` = ligar. Formato atual da API: texto `"0"` ou `"1"`. Aceita também número ou JSON (`{"value":0}`), normal ou escapado; com chave `value`, o valor é lido dela. Vazio, `null`, ausente ou outro valor = inválido, responde `failed`.
- `intertravamento`: `defineIntertravamento()` altera `intertravamento_ativo` (ver `business-rule-intertravamento.md`).
- `fotocelula_1` / `fotocelula_2` (formato atual da API, `action_data` `"0"`/`"1"`): portão vem do tipo da ação (`extraiPortaoFotocelula(tipo_acao, payload)`).
  - `fotocelula` (sem número, formato antigo) continua aceito: `{"port":N,"value":V}`; sem `port` = portão 1; `1` ou `2` = aquele portão; outro número = inválido, responde `failed`.
  - `defineAlimentacaoFotocelula(numero_portao, alimentar)` altera `fotocelula_portao_1_alimentada` (SAÍDA 4, canal 4 / bit 3 do PCA9534) ou `fotocelula_portao_2_alimentada` (SAÍDA 5, canal 5 / bit 4). `0` desenergiza o relé e `1` energiza; a outra fotocélula não muda. A tarefa dos portões aplica em até 10 ms.
- As flags são persistidas na Preferences (`salvaConfiguracaoPortoes()`, chaves `intertravamento`, `fotocelula` (portão 1) e `fotocelula_2` (portão 2), namespace `portguard`) e carregadas no `setup()` (`carregaConfiguracaoPortoes()`, padrão `true`) antes de aplicar as saídas.
- Mesmas proteções do `open_or_close_gate`: sem `queue_id` não aplica; `queue_id` marcado como processado antes de aplicar; `action_status` diferente de `pending` é ignorado. Responde `completed` ao aplicar.
- `processarAlteracaoFlagPortoes(queue_id, tipo_acao, payload)` trata intertravamento, fotocélulas e manutenção.

## Modo manutenção
- Ações `set_maintenance` / `unset_maintenance` também passam por `processarAlteracaoFlagPortoes()`. Ver `business-rule-modo-manutencao.md`.

## Testes
- `teste_fila_api_formato_atual` (em `teste_completo()`): payloads reais do `/list` (id numérico, `fotocelula_2`, `down_siren`, `open_or_close_gate` com `"2"`) e a validade de 60 s das ações imediatas (recente, antiga, sem relógio, no futuro, sem `created_at`).
- `teste_comando_flags_placa` (em `teste_completo()`): formatos de `action_data`, portão por `fotocelula_1`/`fotocelula_2` e leitura do `port`, efeito de cada flag (bloqueio do intertravamento, SAÍDA 4 e SAÍDA 5 LOW/HIGH de forma independente) e persistência salva/recarregada na Preferences. Restaura as flags originais.
- `teste_alimentacao_fotocelula` verifica que cada fotocélula controla só a sua saída e restaura os valores originais das flags, já que podem vir desligadas da Preferences.
