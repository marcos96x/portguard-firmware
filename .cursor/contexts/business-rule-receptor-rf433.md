---
alwaysApply: false
---

# Regra de negócio: receptor RF433 (GPIO48)

A placa MAV1022 possui entrada de receptor 433 MHz no **GPIO48** (sinal invertido pelo transistor Q2, conforme manual). Implementação em `script.cpp`.

## Regras
- A interrupção `CHANGE` no GPIO48 captura o trem de pulsos (durações entre bordas). Intervalo menor que 250 µs ou maior que 1500 µs zera o quadro. Silêncio de 5 ms fecha o quadro. Mais de 96 pulsos descarta o quadro.
- O quadro RF (assinatura FNV-1a, símbolos e botão) só é impresso na janela de 20 s aberta pelo botão GPIO5. Fora dela o receptor não publica esse formato. Quadro de 50 a 70 intervalos e pelo menos 90% curtos (S, 350–650 µs) ou longos (L, 800–1150 µs). Botão 1 = `0xFE280B33`, botão 2 = `0xE5B7B297`, botão 3 = `0x6958C26B`. Outra assinatura aparece como desconhecida. A mensagem repete no máximo a cada 300 ms. Esse aviso não grava controle e não aciona relé.
- Só são aceitos **quadros completos e repetidos** (mesmo código duas vezes seguidas) para cadastro e para abrir o portão. Quadros isolados ou ruído são ignorados nesse caminho.
- O cadastro começa no **botão local GPIO5** (manual: BUTTON, ativo em nível baixo, `pino_botao_cadastrar_controle`). A placa fica 20 s esperando o controle.
- No monitor serial da Arduino IDE aparece o comando assim que o controle é apertado (`codigo 0x...`, quantidade de bits e a sequência binária) e de novo quando o quadro se repete e o cadastro confirma.
- Até 8 controles ficam na Preferences (`rf433_qtd`, `rf433_c0`/`rf433_b0` …). O primeiro também permanece em `rf433_ok`, `rf433_code`, `rf433_bits`.
- **Próximos acionamentos** de um controle que está só na lista binária (`rf433_c*` / `rf433_b*`), sem objeto em `buttons`: a placa trata como comando da receptora do **portão 1**, acionando a **SAÍDA 1** (pulso do motor + janela de autorização), via `tarefaControlePortoes` → `processaPortao` do portão 1.
- Controle cuja **assinatura** foi gravada em `buttons` não usa esse atalho do portão 1. O pulso segue `business-rule-botoes-controle.md`: só com status `vinculado` e `port_number` 1, 2 ou 3.
- Intervalo mínimo de **1 segundo** entre comandos RF433 reconhecidos (evita múltiplos pulsos com o botão mantido).
- Código que não foi cadastrado pelo botão é ignorado (não grava sozinho).

## Estrutura
- `trataInterrupcaoReceptorRf433` / `iniciaReceptorRf433`: captura no GPIO48.
- `classificaIntervaloPulsoRf433` / `calculaAssinaturaSimbolosRf433` / `analisaQuadroAssinaturaRf433`: montam a assinatura e imprimem o quadro RF. `consomeQuadroCapturadoRf433` só chama essa análise com a janela de cadastro aberta.
- `decodificaTremDePulsosRf433`: classifica pulsos curto/longo e monta o código binário.
- `abreCadastroControleRf433`: o botão GPIO5 (ou o teste) abre a janela de cadastro.
- `processaReceptorFrequencia433`: lê o botão, fecha a captura por silêncio, exige repetição, grava se o cadastro estiver aberto ou reconhece um controle já salvo. O comando vai para o monitor serial por `imprimeComandoControleRf433`.
- `salvaCodigoRf433` / `carregaCodigoRf433`: persistência (gravação pendente é salva no `loop()`).
- `carregaConfiguracaoPortoes()` chama `carregaCodigoRf433()` no boot.

## Testes
`teste_receptor_frequencia_433` (em `teste_completo()`): decodificação sintética, rejeição de ruído, recusa de gravar sem o botão, gravação só com quadro repetido depois de `abreCadastroControleRf433`, segundo controle, rejeição de código não cadastrado, reconhecimento acionando o portão 1 e cooldown de 1 s.

`teste_assinatura_quadro_controle_rf433` (em `teste_completo()`): faixas S/L, assinatura FNV, quadros de 50, 57 e 70 intervalos, rejeição fora dessa faixa e abaixo de 90% regulares, nomes dos botões 1, 2 e 3, quadro RF só dentro da janela de cadastro, e garantia de que exibir a assinatura não aciona o portão.
