---
alwaysApply: true
---

# Esquema técnico de componentes e ligações

Placa: **MAV1022** (ESP32-S3). Entradas e saídas de campo NÃO são GPIOs diretos: passam pelos PCA9534 (saídas 0x20, entradas 0x21) no I2C GPIO8/SDA e GPIO9/SCL. Ver `business-rule-hardware-mav1022.md`.

Ethernet W5500 (manual revisão 1.1): MOSI GPIO11, MISO GPIO12, SCK GPIO13, CS GPIO14, INT GPIO10, RST GPIO7. GPIO15/16 ficam no cristal do RTC e GPIO19/20 na USB; não são entrada nem saída de campo.

ENTRADA 1: Receptora 1 - recebe os comandos de abertura e fechamento via controle remoto do portão 1, normalmente LOW (inativo)
ENTRADA 2: Sensor do portão 1 (portão 1 em si) , normalmente HIGH (ativo = fechado)
ENTRADA 3: Receptora 2 - recebe os comandos de abertura e fechamento via controle remoto do portão 2, normalmente LOW (inativo)
ENTRADA 4: Sensor do portão 2 (portão 2 em si) , normalmente HIGH (ativo = fechado)
ENTRADA 5: Receptora 3 - recebe os comandos de abertura e fechamento via controle remoto do portão 3, normalmente LOW (inativo)
ENTRADA 6: Sensor do portão 3 (portão 3 em si) , normalmente HIGH (ativo = fechado)

SAÍDA 1: Motor do portão 1, normalmente LOW
SAÍDA 2: Motor do portão 2, normalmente LOW
SAÍDA 3: Motor do portão 3, normalmente LOW
SAÍDA 4: ligação de interruptor do Sensor de fotocélula do portão 1 para evitar colisão de veículos, normalmente HIGH para o sensor funcionar normalmente
SAÍDA 5: ligação de interruptor do Sensor de fotocélula do portão 2 para evitar colisão de veículos, normalmente HIGH para o sensor funcionar normalmente
SAÍDA 6: ligação de interruptor do Sensor de fotocélula do portão 3 para evitar colisão de veículos, normalmente HIGH para o sensor funcionar normalmente
SAÍDA 7: Sirene, normalmente LOW
SAÍDA 8: segue a lógica exata da sirene (SAÍDA 7), mas normalmente HIGH, para a central do alarme

O portão 3 usa os canais que sobram no PCA9534 (8 entradas e 8 saídas). GPIO do ESP32-S3 permanece no I2C, Ethernet, USB, RTC e RF433.

GPIO5: botão cadastrar controle, ativo em nível baixo. GPIO48: receptor RF433 (sinal invertido). Com o botão pressionado, abre 20 s em que cada tecla imprime o quadro RF no monitor serial, sem acionar relé por esse aviso; o quadro completo e repetido é gravado. A assinatura entra no índice Preferences `buttons` com portão 0 e status livre e o `loop()` envia JSON para `/api/app/v1/plaque-buttons/save`. Cadastrar de novo a mesma assinatura volta para livre/portão 0 e envia `/api/app/v1/plaque-buttons/update` com `control_id` 0 e `port_number` 0. O pulso no portão só ocorre se esse botão estiver `vinculado` ao portão 1, 2 ou 3. Controle gravado só na lista binária antiga, sem entrada em `buttons`, continua acionando a SAÍDA 1 (portão 1). Ver `business-rule-receptor-rf433.md` e `business-rule-botoes-controle.md`.

Wi-Fi no ESP32-S3: sleep do modem desligado + auto-reconnect com credenciais persistidas, para não perder a sessão com o aplicativo. Ver `business-rule-wifi-esp32-s3.md`.

LEDs da placa (GPIO direto, fora do PCA9534): GPIO3 LED azul D6 aceso com Wi-Fi e IP; GPIO21 LED vermelho D7 aceso com cabo Ethernet; GPIO47 LED laranja D8 pisca 1 s aceso e 1 s apagado (status de contato com o servidor). Ver `business-rule-leds-indicadores-rede.md`.
