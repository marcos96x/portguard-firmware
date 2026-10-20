---
alwaysApply: false
---

# Regra de negócio: LEDs de Wi-Fi, cabo e status

Placa **MAV1022** (manual revisão 1.1). Os três LEDs são GPIO direto do ESP32-S3, fora do PCA9534. Nível alto acende o LED. Implementação em `script.cpp`.

## Regras
- **GPIO3, LED azul D6 (Wi-Fi):** aceso enquanto `wifiConectado()` (associado e com IP). Apaga quando a conexão cai.
- **GPIO21, LED vermelho D7 (Ethernet):** aceso enquanto `caboConectado()` (W5500 com IP). Apaga quando o cabo perde o link.
- **GPIO47, LED laranja D8 (status):** pisca o tempo todo, 1 segundo aceso e 1 segundo apagado, no ritmo de contato com o servidor. Não usa `delay`. A tarefa dos portões atualiza o nível a cada ciclo de 10 ms, então a piscada continua mesmo com o HTTP do `loop` bloqueado.
- A mudança do Wi-Fi ou do cabo gera uma linha no monitor serial. A piscada do status não é logada.

## Funções
- `nivelLedConexao()`, `nivelLedStatusServidor()`, `atualizaLedsIndicadoresRede()`.
- `configuraPinos()` coloca os três pinos em saída e começa com os LEDs apagados.
- `tarefaControlePortoes()` chama `atualizaLedsIndicadoresRede()` a cada ciclo.

## Testes
`teste_leds_indicadores_rede` (em `teste_completo()`): GPIOs 3, 21 e 47 sem conflito com I2C, Ethernet, RF, USB ou RTC; LED de link alto só com conexão; LED de status alto de 0 a 999 ms, baixo de 1000 a 1999 ms e alto de novo em 2000 ms.
