# Comunicação com a API, Wi-Fi salvo e fila de logs

Implementada em `script.cpp`.

## Endereço
- A placa fala com `https://portguard.com.br/api/v1` e `https://portguard.com.br/api/app/v1`.

## connection_type
- Todo log enviado para `/plaque-events-logs/save` ou `/api/app/v1/plaques/logs` leva `connection_type`.
- O valor é o meio que está no ar no instante do envio: `wifi` ou `ethernet`.
- Se os dois estão no ar, vai `wifi`, porque o envio tenta o Wi-Fi primeiro.
- O campo não entra na fila. Ele é colocado na hora em que o log sai.

## last_wifi_config
- Quando uma configuração de Wi-Fi associa de verdade, SSID e senha vão para a Preferences no JSON `{"ssid":"...","password":"..."}`.
- A chave pedida `last_wifi_config` tem 16 caracteres. O NVS só grava 15, então a chave usada é `last_wifi_confi`.
- Uma tentativa que ainda não conectou não atualiza esse JSON.
- Placa que já tinha Wi-Fi salvo e ainda não tem esse JSON: na carga, a configuração salva preenche `last_wifi_config`.
- `reset_plaque` apaga esse JSON junto com o Wi-Fi.

## Configuração que não conecta
- O prazo continua `tempo_sem_wifi_para_restaurar_ultimo_online_ms` (120000 ms).
- Se nesse prazo o rádio não associa no SSID novo, a placa lê `last_wifi_config`, faz `WiFi.begin` nessa rede e marca a ação como `failed`.
- Sem esse JSON, usa a cópia `wifi_ssid_ok` / `wifi_senha_ok`. Sem nenhuma das duas, a tentativa é rejeitada.
- A placa envia um log `wifi_error`. O `event_data` é o SSID e a senha da rede que falhou, não os da rede restaurada.
- A senha não aparece no `print_log`.

## queue_logs
- Log que não sai porque não há internet (sem rede, ou a requisição não recebe resposta HTTP) entra na Preferences `queue_logs`, em JSON.
- Resposta HTTP de erro não entra na fila: a internet existia.
- Heartbeat não entra na fila.
- Descarte por manutenção ou por falta de serial também não entra.
- A fila guarda no máximo cerca de 2800 caracteres. Acima disso, o log mais antigo sai.
- Quando a internet volta, `queue_logs` é enviada antes da consulta da fila, do heartbeat e dos outros envios. Cada item recebe o `connection_type` daquele momento.
- Se o envio da fila cair de novo por falta de internet, o que não saiu continua gravado.
- Depois que a fila sai, `queue_logs` fica `[]`.

## Teste
- `teste_comunicacao_api_wifi_e_fila_logs` em `teste_completo()`: URL, `connection_type`, chave de 15 caracteres, JSON do Wi-Fi, `wifi_error` com a rede que falhou e limite da fila. Não grava Preferences nem chama a rede.
