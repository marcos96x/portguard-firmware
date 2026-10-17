---
alwaysApply: false
---

# Regra de negócio: Wi-Fi estável no ESP32-S3 (manter app online)

Placa **MAV1022 / ESP32-S3**. Sem estes ajustes o modem entra em sleep e/ou o firmware reinicia `WiFi.begin` com frequência demais, derrubando o HTTPS com a API e a sensação de “perda de conexão com o aplicativo”.

## Regras
- Com credenciais **já persistidas e válidas** (`setup_finished`, SSID preenchido, sem `wifiCredenciaisPendentes` e sem `wifiConfigFalhou`):
  - `WiFi.setSleep(false)` — modem sempre acordado.
  - `WiFi.setAutoReconnect(true)` — a pilha reconecta sozinha.
  - `WiFi.persistent(false)` — evita gravar NVS a cada begin.
  - Não chama `disconnect` + `begin` a cada ciclo; no máximo `WiFi.reconnect()` a cada `INTERVALO_RECONECTAR_WIFI_MS` (5 s) se ainda estiver offline.
- Com **config_plaque** pendente: uma tentativa só, `setAutoReconnect(false)` (igual à regra antiga de validação da fila).
- Enquanto o Wi-Fi estiver com IP: `Network.setDefaultInterface(WiFi.STA)` — prioridade sobre o cabo W5500 para as requisições do app.
- Eventos `ARDUINO_EVENT_WIFI_STA_GOT_IP` / `DISCONNECTED` reaplicam sleep off e auto-reconnect.

## Funções
- `deveManterReconexaoAutomaticaWifi()`, `configuraEstabilidadeWifiEsp32S3()`, `registraEventosWifiEsp32S3()`, `onWifiEvent()`, `conectarWifi()`, `verificarRede()`, `aplicarRotaDeRede()`.

## Testes
`teste_estabilidade_wifi_esp32_s3` (em `teste_completo()`): intervalos, política de auto-reconnect (persistido / pendente / rejeitado / sem SSID) e aplicação de `configuraEstabilidadeWifiEsp32S3`.
