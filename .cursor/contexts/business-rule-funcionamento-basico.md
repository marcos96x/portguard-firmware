---
alwaysApply: false
---

# Regra de negócio: funcionamento básico da placa (2 portões)

Lógica local que mantém a placa operando de forma autônoma, com os portões 1 e 2 independentes. Implementada em `script.cpp` (placa **MAV1022** / ESP32-S3). Detalhe do hardware em `business-rule-hardware-mav1022.md`.

## Pinagem (canais do PCA9534, não GPIOs diretos)
| Função | Canal (bit) | Repouso lógico |
|---|---|---|
| ENTRADA 1 - Receptora portão 1 | 1 (bit 0) | inativo |
| ENTRADA 2 - Sensor portão 1 | 2 (bit 1) | ativo = fechado |
| ENTRADA 3 - Receptora portão 2 | 3 (bit 2) | inativo |
| ENTRADA 4 - Sensor portão 2 | 4 (bit 3) | ativo = fechado |
| SAÍDA 1 - Motor portão 1 | 1 (bit 0) | desenergizado |
| SAÍDA 2 - Motor portão 2 | 2 (bit 1) | desenergizado |
| SAÍDA 3 - Sirene | 3 (bit 2) | desenergizado |
| SAÍDA 4 - Fotocélula portão 1 | 4 (bit 3) | energizado |
| SAÍDA 5 - Fotocélula portão 2 | 5 (bit 4) | energizado |
| SAÍDA 6 - Central de alarme (inverso da SAÍDA 3) | 6 (bit 5) | energizado |

Entradas/saídas via PCA9534 (0x21 / 0x20) no I2C GPIO8/GPIO9. Ethernet W5500: MOSI11, MISO12, SCK13, CS14, INT10, RST7.

## Regras (aplicadas por portão)
- Entradas passam por estabilização de 50 ms antes de serem consideradas.
- Borda de subida estável da receptora: pulso de 1 s no motor do portão.
  - Portão fechado: abre janela de autorização de 3 s.
  - Portão aberto: é comando de fechamento, não abre janela.
  - Intertravamento: abertura ignorada se o outro portão estiver aberto (ver `business-rule-intertravamento.md`).
- Sensor passa a LOW (portão abriu): evento `gate_open`.
  - Dentro da janela de autorização: abertura autorizada (consome a autorização e zera o contador de falha).
  - Sem autorização: evento `gate_broken` e sirene contínua enquanto o portão estiver aberto. O `gate_broken` vai para a API no máximo 1 vez por minuto (ver `business-rule-limite-gate-broken.md`).
  - Ao fechar após abertura sem autorização, a sirene continua contínua por mais 120 s (`tempo_sirene_apos_fechamento_abertura_indevida_ms`), mesmo que o portão tenha sido fechado logo após abrir. Nova abertura indevida nesse intervalo volta ao alarme de portão aberto e reinicia os 120 s no próximo fechamento.
- Autorização expirada sem abertura: contador de possível falha do sensor; no 6º seguido envia `need_maintenance`.
- Portão aberto por 60 s: evento `gate_timeout_open` e sirene intermitente (1 s ligada / 2 s desligada) até fechar.
- Portão fecha: evento `gate_close`, encerra alarmes daquele portão (exceto a sirene pós-fechamento de abertura indevida) e envia `gate_timeout_close` se havia alerta de tempo.
- Sirene liga se qualquer portão tiver alarme; abertura indevida (aberta ou nos 120 s pós-fechamento) tem prioridade (contínua) sobre a intermitente.
- A ação `down_siren` da fila desliga a sirene (ver `business-rule-desligar-sirene.md`).
- SAÍDA 6 é sempre o inverso da SAÍDA 3.
- SAÍDAS 4 e 5 (fotocélulas dos portões 1 e 2) ficam em HIGH, independente da sirene e de qualquer sensor; só mudam pelas ações `fotocelula_1` / `fotocelula_2` da fila, cada uma com sua flag persistida (ver `business-rule-configuracao-remota-placa.md`).
- O estado das entradas na inicialização é tomado como referência e não gera eventos.

## Estrutura
- `tarefaControlePortoes()` (FreeRTOS, prioridade 2, a cada 10 ms) lê entradas, chama `processaPortao()` para cada portão e `aplicaSaidas(calculaSaidas(...))`. Fica separada do `loop()` para que HTTP bloqueante não atrase comandos, sensores e sirene.
- `processaPortao()` só altera o `EstadoPortao`; `calculaSaidas()` calcula os níveis; `aplicaSaidas()` é o único ponto que escreve nas saídas.
- Eventos vão para `fila_eventos_pendentes` (`registraEventoPortao()`); o `loop()` envia com `enviaEventosPendentes()` usando `event_data={"port":N}`. Sem rede os eventos aguardam na fila (até 20).
- `EstadoPortao.modo_teste = true` guarda eventos em `eventos_registrados_teste` em vez de enviar à API.

## Testes
`teste_configuracao_pinos`, `teste_estabilizacao_entrada`, `teste_comando_autorizado`, `teste_comando_com_portao_aberto`, `teste_abertura_indevida`, `teste_portao_aberto_tempo_excessivo`, `teste_validade_comando_autorizado`, `teste_independencia_portoes`, `teste_alimentacao_fotocelula`, `teste_saida_fotocelula_e_central_alarme` — todos chamados em `teste_completo()`, executado no fim do `setup()` antes de iniciar a tarefa dos portões.
