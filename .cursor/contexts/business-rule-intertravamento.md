---
alwaysApply: false
---

# Regra de negócio: intertravamento dos portões

Impede que um portão seja aberto enquanto qualquer outro já está aberto. Vale para os três portões. Implementada em `script.cpp`, dentro de `processaPortao()`.

## Regras
- Controlada pela variável global `intertravamento_ativo` (padrão `true`). Com `false` os portões voltam a ser totalmente independentes. Alterada remotamente pela ação `intertravamento` da fila (0/1) e persistida na Preferences (ver `business-rule-configuracao-remota-placa.md`).
- "Portão aberto" vem do sensor estabilizado (ENTRADA 2 / ENTRADA 4 / ENTRADA 6 em LOW, conforme `architecture.md`). Os três sensores usam a mesma polaridade: opto ativo = portão fechado.
- Placa de 1 portão não intertrava. Placa de 2 portões só considera os portões 1 e 2. Placa de 3 portões considera os três. A quantidade vem do serial (`business-rule-quantidade-portoes.md`).
- Portão em manutenção não bloqueia os outros e a abertura dele não é bloqueada.
- Comando de **abertura** (borda de subida da receptora com o próprio portão fechado) é ignorado se qualquer outro portão existente, fora de manutenção, estiver aberto:
  - não gera pulso no motor e não abre janela de autorização;
  - registra log "Comando de abertura ignorado pelo intertravamento";
  - o comando é descartado: depois que o outro portão fechar é preciso acionar o controle novamente.
  - se o portão abrir mesmo assim, é tratado como abertura indevida (`gate_broken` + sirene).
- Comando de **fechamento** (receptora com o próprio portão aberto) nunca é bloqueado.
- O intertravamento olha somente os sensores: um portão com comando autorizado que ainda não abriu fisicamente (dentro da janela de 3 s) não bloqueia o outro.

## Estrutura
- `processaPortao(portao, receptora_acionada, sensor_fechado, agora, outro_portao_aberto, comando_remoto)`: `outro_portao_aberto` (padrão `false`) informa se algum outro portão está aberto. Retorna `false` quando um comando foi bloqueado pelo intertravamento.
- O intertravamento vale também para o comando remoto `open_or_close_gate` (ver `business-rule-comando-remoto-portao.md`).
- `tarefaControlePortoes()` usa `sensorBloqueiaIntertravamento()`: só sensor aberto de portão que existe nesta placa e que não está em manutenção.

## Testes
`teste_intertravamento` (chamado em `teste_completo()`): abertura do portão 1 bloqueia o portão 2 e abertura do portão 2 bloqueia o portão 1; arrombamento do portão 2 gera `gate_broken` e sirene; abertura forçada após comando ignorado gera `gate_broken`; fechamento liberado; abertura liberada com o outro fechado e com `intertravamento_ativo = false`. Restaura o valor original de `intertravamento_ativo` ao final.
`teste_intertravamento_tres_portoes` (chamado em `teste_completo()`): portão 3 aberto bloqueia a abertura dos portões 1 e 2; portão 1 ou 2 aberto bloqueia a abertura do portão 3; fechamento do portão 1 continua liberado com o portão 3 aberto; abertura forçada do portão 3 gera `gate_broken`, liga a sirene (SAÍDA 7, bit 6) e desliga o relé da central (SAÍDA 8, bit 7).
