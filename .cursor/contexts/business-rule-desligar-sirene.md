---
alwaysApply: false
---

# Regra de negócio: desligar a sirene via API (`down_siren`)

A consulta da fila (`buscarConfigWifiNaApi()` via `/plaque-queues-actions/list`, a cada 500 ms) trata a ação `down_siren` (`action_data` vazio). Implementada em `script.cpp`.

## Regras
- Desliga a sirene dos dois portões (`desligaSirenePortao()` em cada um):
  - encerra o alarme de abertura indevida, a sirene pós-fechamento (120 s) e o alerta de tempo aberto;
  - SAÍDA 3 vai para LOW e a SAÍDA 6 (central de alarme) volta para HIGH no ciclo seguinte da tarefa (até 10 ms);
  - com o portão ainda aberto, a contagem de tempo aberto recomeça do zero: o alerta intermitente só volta depois de mais `tempo_maximo_portao_aberto_ms` (60 s) aberto;
  - fechar o portão depois do `down_siren` não religa a sirene pós-fechamento;
  - uma nova abertura sem autorização volta a disparar a sirene e o `gate_broken` normalmente.
- Responde `completed` ao executar; `failed` se a tarefa dos portões não responder.
- `down_siren` criado há mais de 60 s não é executado (responde `failed`), para que um comando antigo acumulado na fila não cale um alarme novo (ver "Ações imediatas antigas" em `business-rule-configuracao-remota-placa.md`).
- O `queue_id` é marcado como processado antes de executar; sem `queue_id` não executa.

## Estrutura
- `processarDesligarSirene(queue_id)` roda no `loop()` e usa `enviaComandoRemotoParaTarefaPortoes(comando_remoto_desligar_sirene, ...)` (código 3 na `fila_comandos_remotos_portao`).
- `tarefaControlePortoes()` recebe o código 3 e chama `desligaSirenePortao()` nos dois portões, sem concorrência com o restante da lógica dos portões.

## Testes
`teste_desligar_sirene_remoto` (em `teste_completo()`): abertura indevida em um portão e alerta de tempo aberto no outro; após o `down_siren` a sirene desliga (SAÍDA 6 em HIGH), não volta logo em seguida com o portão aberto, volta após um novo tempo máximo, não há sirene pós-fechamento e nova abertura indevida rearma o alarme.
