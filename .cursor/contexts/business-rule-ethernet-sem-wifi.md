---
alwaysApply: false
---

# Regra de negócio: cabo assume a comunicação quando não há Wi-Fi

Placa **MAV1022 / ESP32-S3**. O aplicativo mostra “aguardando configuração” enquanto o status da placa na API é `need_config` ou `waiting_config`. Esse status só vira `active` quando a placa responde `config_plaque` com `completed`. Sem Wi-Fi persistido, o cabo com IP faz esse papel e carrega heartbeat, fila e comandos.

## Regras
- Com Wi-Fi associado e com IP, as requisições continuam saindo pelo Wi-Fi. Nada da configuração de Wi-Fi já persistida muda.
- Sem Wi-Fi persistido (`setup_finished` falso) e com o cabo com IP:
  - a rota padrão é a Ethernet;
  - o rádio Wi-Fi não é ligado só para consultar o status (no ESP32-S3 isso tira a rota do cabo);
  - `config_plaque` pendente, com ou sem SSID, responde `completed` na hora. Se a ação trouxer SSID, a tentativa única de Wi-Fi continua; se o Wi-Fi conectar, as credenciais são persistidas como antes; se não conectar, o cabo permanece e a ação não volta para `failed`.
- Sem cabo e sem Wi-Fi, `config_plaque` segue `failed`.
- O teste de estabilidade do Wi-Fi não liga nem desliga o rádio quando a Ethernet já foi iniciada e não há credencial persistida. `WiFi.mode(WIFI_OFF)` depois do W5500 derruba o cabo. A tentativa de `config_plaque`, a falha dela e o `reset_plaque` usam `WiFi.disconnect(false)` enquanto a Ethernet está no ar, pelo mesmo motivo.

## Funções
- `wifiConectado()`, `deveTentarWifiAntesDoCabo()`, `statusDaConfiguracaoQuandoWifiNaoSobe()`, `caboDeveAssumirConfiguracaoDoAplicativo()`, `confirmaConfiguracaoPeloCabo()`.

## Testes
`teste_cabo_assume_comunicacao_sem_wifi` (em `teste_completo()`): cabo confirma a configuração só sem Wi-Fi no ar e sem Wi-Fi persistido; sem cabo a ação falha; sem credencial a requisição não escolhe o Wi-Fi; credencial persistida ou `config_plaque` pendente ainda tenta o Wi-Fi antes; consultar o Wi-Fi não liga o rádio.
