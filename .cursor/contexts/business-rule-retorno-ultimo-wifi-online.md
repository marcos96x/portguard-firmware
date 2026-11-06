# Volta ao último Wi-Fi que deixou a placa online

Implementada em `script.cpp`.

## O que fica guardado
- Quando o Wi-Fi conecta de verdade, SSID e senha são copiados para a Preferences `wifi_ssid_ok` e `wifi_senha_ok` (namespace `portguard`).
- Essa cópia não muda numa `config_plaque` que ainda não conectou.
- Placa que já tinha Wi-Fi salvo e ainda não tem essa cópia: na carga, a configuração salva vira o último Wi-Fi online.
- `reset_plaque` apaga também essa cópia.

## Troca errada no aplicativo
- Antes do `WiFi.begin` da rede nova, a placa lê do rádio o SSID e a senha aos quais já está associada (`esp_wifi_sta_get_ap_info` e `esp_wifi_get_config`) e guarda em `wifi_ssid_ok` / `wifi_senha_ok`.
- `WiFi.SSID()` não decide essa cópia: ele pode devolver a rede apenas configurada, ainda sem associação.
- A tentativa nova faz um `WiFi.begin` com o SSID e a senha recebidos. Não usa `WiFi.reconnect()`, que religava a rede antiga do rádio e impedia a volta.
- O prazo é `tempo_sem_wifi_para_restaurar_ultimo_online_ms` (120000 ms), contado de `momentoInicioConfigWifi`. Se nesse prazo o rádio não associar no SSID novo, a placa chama `WiFi.begin` na rede guardada. Não precisa de cabo.
- A `config_plaque` que não conectou fica marcada para resposta `failed` quando a rede antiga voltar.
- A senha nova só é gravada quando a placa associa de fato nesse SSID.

## Queda de energia
- Se a credencial em uso já é a que deixou a placa online, a conexão começa na hora, no fluxo normal de reconexão.
- Se a credencial em uso for outra (troca que chegou a ser salva), os mesmos 2 minutos offline devolvem o Wi-Fi memorizado.

## Sem Wi-Fi anterior
- Primeira configuração que não conecta em 2 minutos segue rejeitada, sem apagar um Wi-Fi que não existe.

## Teste
- `teste_retorno_ultimo_wifi_online` em `teste_completo()`: 2 minutos, não gravar senha nova no SSID antigo, volta da credencial memorizada e estouro do `millis`. Não grava Preferences nem chama o rádio.

## last_wifi_config
- A configuração que conectou também fica no JSON `last_wifi_config` (chave NVS `last_wifi_confi`).
- Se a rede nova não associa em 2 minutos, a volta usa esse JSON e a placa envia `wifi_error` com o SSID e a senha que falharam.
- A cópia `wifi_ssid_ok` / `wifi_senha_ok` continua sendo a da queda de energia. As duas só mudam com uma conexão confirmada.
