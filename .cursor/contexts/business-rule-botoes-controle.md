---
alwaysApply: false
---

# Regra de negócio: botões de controle (cadastro, vínculo e pulso)

A placa guarda os botões de controle no índice `buttons` da Preferences (namespace `portguard`) e usa a assinatura RF433 como `button_id`. Implementada em `script.cpp`.

## Armazenamento
- Chave `buttons`: array JSON de objetos.
- Cada objeto: `{"button_id": "XXXX", "port_number": "0", "status": "livre"}`.
- `button_id` é a assinatura FNV do quadro, gravada em decimal (o mesmo número da assinatura `0x...` do monitor serial).
- `port_number`: `"0"`, `"1"`, `"2"` ou `"3"`.
- `status`: `"livre"`, `"vinculado"` ou `"desativado"`.
- Até 20 botões. A lista em memória é a que a tarefa consulta; o `loop()` grava a Preferences.

## Cadastro no botão GPIO5
- A janela de 20 s continua a do receptor (`business-rule-receptor-rf433.md`).
- Quadro repetido com assinatura válida acrescenta um objeto com `port_number` `"0"` e `status` `"livre"`.
- Assinatura já presente não é duplicada. O objeto volta para `port_number` `"0"` e `status` `"livre"`.
- O `loop()` envia o cadastro novo em `POST /api/app/v1/plaque-buttons/save`.
- Corpo JSON, `Content-Type: application/json`: `internal_token`, `serial_id` (serial da placa) e `button_id` numérico. A placa não tem `login_hash` nem `plaque_id`; a rota aceita `serial_id` no lugar da placa.
- No re-cadastro, o `loop()` envia `POST /api/app/v1/plaque-buttons/update` com o mesmo JSON e mais `control_id` `0` e `port_number` `0`, soltando o controle.
- A pendência (`api_pendente`: `cadastrar` ou `resetar`) fica no JSON de `buttons`. Sem confirmação HTTP 200/201, a tentativa repete a cada 5 s. O objeto permanece na placa.
- Código binário sem assinatura segue só a lista antiga e, nas próximas pressões, abre o portão 1.

## Ação `update_button_control`
- Chega na consulta periódica `/api/v1/plaque-queues-actions/list` (não expira em 60 s).
- `action_data`: `{"button_id": xxx, "port_number": 0|1|2|3, "status": "livre"|"vinculado"|"desativado"}`.
- Pode trazer um objeto ou vários no mesmo `action_data`. Cada um é aplicado.
- Aceita número ou texto, JSON normal ou escapado (inclusive escapado mais de uma vez), e `button_id` em decimal ou hexadecimal (`0x...`).
- Antes de alterar, recoloca na lista em uso um botão que esteja só na Preferences, sem apagar os que já estão na memória.
- Localiza o objeto com a mesma assinatura, atualiza `status` e `port_number`, cancela cadastro ou reset ainda pendente para a API e regrava `buttons`.
- A gravação usa um buffer fixo com a lista inteira. A pendência só acaba quando a leitura da Preferences devolve o mesmo JSON. Gravação cortada não substitui a lista.
- Sem `queue_id`, com dado inválido ou com botão ausente: responde `failed`. Botão ausente não é criado. Os outros objetos do mesmo `action_data` que existirem são gravados.
- Se a Preferences não confirmar, a fila não é respondida na hora e o mesmo evento é tentado de novo. Depois de 5 tentativas responde `failed` para a fila seguir; a lista em uso continua e a gravação segue pendente.
- `action_status` diferente de `pending` é ignorado. Sucesso de todos os objetos responde `completed`.
- No serial, cada evento mostra o `action_data`, se o botão foi localizado, se a Preferences confirmou a gravação e se o botão pode enviar o comando de abertura.

## Pulso do controle
- Quadro repetido cuja assinatura está em `buttons`:
  - `vinculado` e `port_number` 1, 2 ou 3, e o portão existe na placa: um pulso nesse portão (mesma regra da receptora, inclusive intertravamento e janela de 13 s). Se o intertravamento bloquear a abertura, o serial diz qual botão e qual portão foram barrados.
  - `livre`, `desativado` ou portão 0: não pulsa.
- Esse pulso não usa o atalho antigo do portão 1.
- Assinatura que não está em `buttons` e o código binário está na lista antiga: continua abrindo o portão 1.
- Intervalo mínimo de 1 s entre comandos reconhecidos.
- Se o pulso foi executado, o `loop()` envia `POST /api/app/v1/plaques/logs` com `internal_token`, `serial`, `serial_id`, `event_type` (`gate_open` se o portão estava fechado, `gate_close` se estava aberto), `button_id` e `port_number` do objeto. Pulso bloqueado pelo intertravamento não gera esse log.
- O evento do sensor (`gate_open` / `gate_close` em `/plaque-events-logs/save`) continua igual.

## Testes
`teste_gerenciamento_botoes_controle` (em `teste_completo()`): JSON do índice `buttons`, cadastro livre sem pulso, re-cadastro que reseta para livre/portão 0 sem duplicar, persistência de `api_pendente`, recusa de dado inválido, leitura do `action_data`, vínculo no portão 3, pulso único, botão desativado sem pulso, os três `update_button_control` em sequência (1767424619, 3854021271 e 4264037171) com ida e volta do JSON sem perder os anteriores, os três no mesmo `action_data`, o pulso bloqueado pelo intertravamento e liberado com o outro portão fechado, e os corpos JSON de `/plaque-buttons/save`, `/plaque-buttons/update` e o log em `/plaques/logs`. Restaura a lista que estava na memória. Não grava a Preferences de teste por cima dos botões da placa.
