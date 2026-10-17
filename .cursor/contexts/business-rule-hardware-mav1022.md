---
alwaysApply: false
---

# Regra de negócio: hardware MAV1022 (ESP32-S3)

A placa alvo do firmware passou a ser a **MAV1022** (manual de uso revisão 1.1). Implementação em `script.cpp`.

## Diferença em relação à HF-006Enet
Na HF-006Enet cada entrada/saída era um GPIO direto do ESP32. Na MAV1022 as 8 entradas e 8 saídas passam por expansores **PCA9534** no I2C principal — **não existem GPIOs de I/O digital de campo no ESP32**.

## Barramentos (manual)
| Função | GPIO / endereço |
|---|---|
| I2C SDA | GPIO8 |
| I2C SCL | GPIO9 |
| PCA9534 saídas | 0x20 |
| PCA9534 entradas | 0x21 |
| W5500 MOSI | GPIO11 |
| W5500 MISO | GPIO12 |
| W5500 SCK | GPIO13 |
| W5500 CS | GPIO14 |
| W5500 INT | GPIO10 |
| W5500 RST | GPIO7 |

## Polaridade (manual)
- Entrada optoacoplada **ativa** → bit correspondente do PCA9534 em **nível baixo**.
- Saída: firmware usa **bit lógico 1** para relé **energizado** (PCA9534 alto → TBD62083 conduz a bobina ao GND).

## Canais usados pelo PortGuard
Canal da placa N = bit (N−1) do expansor (P0..P7). Confirmar na serigrafia se algum canal parecer invertido.

| Função PortGuard | Canal | Bit |
|---|---|---|
| ENTRADA 1 receptora portão 1 | 1 | 0 |
| ENTRADA 2 sensor portão 1 | 2 | 1 |
| ENTRADA 3 receptora portão 2 | 3 | 2 |
| ENTRADA 4 sensor portão 2 | 4 | 3 |
| SAÍDA 1 motor portão 1 | 1 | 0 |
| SAÍDA 2 motor portão 2 | 2 | 1 |
| SAÍDA 3 sirene | 3 | 2 |
| SAÍDA 4 fotocélula portão 1 | 4 | 3 |
| SAÍDA 5 fotocélula portão 2 | 5 | 4 |
| SAÍDA 6 central de alarme | 6 | 5 |

Repouso lógico (igual à regra de negócio geral): motores/sirene desenergizados; fotocélulas e central de alarme energizadas (HIGH lógico = bit 1).

## Receptor RF433
- **GPIO48** — entrada do receptor 433 MHz (sinal invertido por Q2). Não passa pelo PCA9534.
- Aprendizado e acionamento da SAÍDA 1: ver `business-rule-receptor-rf433.md`.

## Funções
- `configuraPinos()`: I2C 100 kHz em GPIO8/9, polaridade 0x02=0x00, PCA9534 0x20 saída / 0x21 entrada, confirma escrita lendo de volta; `pinMode(GPIO48, INPUT)`.
- `leRegistradorPca9534ComSucesso()`: **STOP + requestFrom** (sem `endTransmission(false)`). No ESP32-S3 o repeated-start do Wire falha e deixava sensores/sirene mortos.
- `leEntradasPca9534()` / `canalEntradaAtivo()`: leitura e polaridade ativa-baixa; loga mudança das ENTRADAS 1–4.
- `montaByteSaidasPca9534()` / `aplicaSaidas()`: empacota os 6 níveis e **regrava sempre** o byte no 0x20 (sirene/motores não ficam presos após glitch).
- `iniciaReceptorRf433()`: `attachInterrupt` no GPIO48 após os testes do `setup()`.
- Tarefa dos portões: pilha 12288 bytes (evita crash que matava I/O).

## Testes
`teste_configuracao_pinos`, `teste_comunicacao_pca9534` (write/readback da sirene + polaridade do sensor) e `teste_saida_fotocelula_e_central_alarme`. `teste_receptor_frequencia_433` cobre o GPIO48 / RF433.
