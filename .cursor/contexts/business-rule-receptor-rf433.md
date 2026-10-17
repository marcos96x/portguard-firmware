---
alwaysApply: false
---

# Regra de negócio: receptor RF433 (GPIO48)

A placa MAV1022 possui entrada de receptor 433 MHz no **GPIO48** (sinal invertido pelo transistor Q2, conforme manual). Implementação em `script.cpp`.

## Regras
- A interrupção `CHANGE` no GPIO48 captura o trem de pulsos (durações entre bordas).
- Só são aceitos **quadros completos e repetidos** (mesmo código duas vezes seguidas). Quadros isolados ou ruído são ignorados.
- **Primeiro sinal válido** (ainda sem código gravado): o código é aprendido e persistido na Preferences (`rf433_ok`, `rf433_code`, `rf433_bits`).
- **Próximos acionamentos** com o mesmo código: a placa trata como comando da receptora do **portão 1**, acionando a **SAÍDA 1** (pulso do motor + janela de autorização), via `tarefaControlePortoes` → `processaPortao` do portão 1.
- Intervalo mínimo de **1 segundo** entre comandos RF433 reconhecidos (evita múltiplos pulsos com o botão mantido).
- Código diferente do gravado é ignorado (não sobrescreve o aprendizado automático).

## Estrutura
- `trataInterrupcaoReceptorRf433` / `iniciaReceptorRf433`: captura no GPIO48.
- `decodificaTremDePulsosRf433`: classifica pulsos curto/longo e monta o código binário.
- `processaReceptorFrequencia433`: fecha a captura por silêncio, exige repetição, grava ou reconhece.
- `salvaCodigoRf433` / `carregaCodigoRf433`: persistência (gravação pendente é salva no `loop()`).
- `carregaConfiguracaoPortoes()` chama `carregaCodigoRf433()` no boot.

## Testes
`teste_receptor_frequencia_433` (em `teste_completo()`): decodificação sintética, rejeição de ruído, gravação só com quadro repetido, rejeição de código diferente, reconhecimento acionando o portão 1 e cooldown de 1 s.
