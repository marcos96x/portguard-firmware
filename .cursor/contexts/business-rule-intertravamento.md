---
alwaysApply: false
---

# Regra de negócio: intertravamento dos portões

Impede que um portão seja aberto enquanto o outro já está aberto. Implementada em `script.cpp`, dentro de `processaPortao()`.

## Regras
- Controlada pela variável global `intertravamento_ativo` (padrão `true`). Com `false` os portões voltam a ser totalmente independentes. Alterada remotamente pela ação `intertravamento` da fila (0/1) e persistida na Preferences (ver `business-rule-configuracao-remota-placa.md`).
- "Portão aberto" vem do sensor estabilizado (ENTRADA 2 / ENTRADA 4 em LOW, conforme `architecture.md`).
- Comando de **abertura** (borda de subida da receptora com o próprio portão fechado) é ignorado se o outro portão estiver aberto:
  - não gera pulso no motor e não abre janela de autorização;
  - registra log "Comando de abertura ignorado pelo intertravamento";
  - o comando é descartado: depois que o outro portão fechar é preciso acionar o controle novamente.
  - se o portão abrir mesmo assim, é tratado como abertura indevida (`gate_broken` + sirene).
- Comando de **fechamento** (receptora com o próprio portão aberto) nunca é bloqueado.
- O intertravamento olha somente os sensores: um portão com comando autorizado que ainda não abriu fisicamente (dentro da janela de 3 s) não bloqueia o outro.

## Estrutura
- `processaPortao(portao, receptora_acionada, sensor_fechado, agora, outro_portao_aberto, comando_remoto)`: `outro_portao_aberto` (padrão `false`) informa se o outro portão está aberto. Retorna `false` quando um comando foi bloqueado pelo intertravamento.
- O intertravamento vale também para o comando remoto `open_or_close_gate` (ver `business-rule-comando-remoto-portao.md`).
- `tarefaControlePortoes()` passa `!sensor_portao_2_fechado` para o portão 1 e `!sensor_portao_1_fechado` para o portão 2, usando as leituras estabilizadas do mesmo ciclo.

## Testes
`teste_intertravamento` (chamado em `teste_completo()`): abertura bloqueada com o outro portão aberto, abertura forçada após comando ignorado gera `gate_broken`, fechamento liberado nos dois portões, abertura liberada com o outro fechado e com `intertravamento_ativo = false`. Restaura o valor original de `intertravamento_ativo` ao final.
