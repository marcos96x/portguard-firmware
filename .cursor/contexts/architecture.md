---
alwaysApply: true
---

# Esquema técnico de componentes e ligações

Placa: **MAV1022** (ESP32-S3). Entradas e saídas de campo NÃO são GPIOs diretos: passam pelos PCA9534 (saídas 0x20, entradas 0x21) no I2C GPIO8/SDA e GPIO9/SCL. Ver `business-rule-hardware-mav1022.md`.

ENTRADA 1: Receptora 1 - recebe os comandos de abertura e fechamento via controle remoto do portão 1, normalmente LOW (inativo)
ENTRADA 2: Sensor do portão 1 (portão 1 em si) , normalmente HIGH (ativo = fechado)
ENTRADA 3: Receptora 2 - recebe os comandos de abertura e fechamento via controle remoto do portão 2, normalmente LOW (inativo)
ENTRADA 4: Sensor do portão 2 (portão 2 em si) , normalmente HIGH (ativo = fechado)

SAÍDA 1: Motor do portão 1, normalmente LOW
SAÍDA 2: Motor do portão 2, normalmente LOW
SAÍDA 3: Sirene, normalmente LOW
SAÍDA 4: ligação de interruptor do Sensor de fotocélula do portão 1 para evitar colisão de veículos, normalmente HIGH para o sensor funcionar normalmente
SAÍDA 5: ligação de interruptor do Sensor de fotocélula do portão 2 para evitar colisão de veículos, normalmente HIGH para o sensor funcionar normalmente
SAÍDA 6: segue a lógica exata da SAÍDA 3 (sirene), mas normalmente HIGH

GPIO48: receptor RF433 (sinal invertido). Primeiro quadro completo e repetido é gravado; nas próximas pressões iguais do controle, aciona a SAÍDA 1 (portão 1). Ver `business-rule-receptor-rf433.md`.

Wi-Fi no ESP32-S3: sleep do modem desligado + auto-reconnect com credenciais persistidas, para não perder a sessão com o aplicativo. Ver `business-rule-wifi-esp32-s3.md`.
