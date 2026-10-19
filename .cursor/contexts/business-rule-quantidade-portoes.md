---
alwaysApply: false
---

# Regra de negócio: quantidade de portões da placa

Implementada em `script.cpp`. O mesmo firmware atende placa de 1, 2 ou 3 portões.

## Regras
- A quantidade desta placa é `quantidade_portoes_configurada` (3). O serial da API (`1port-...`) só identifica a placa e não desliga o portão 3.
- Um rótulo curto `1port`, `2port` ou `3port` (sem o código depois do traço) ainda pode ser lido por `quantidadePortoesDoSerial()`. O serial completo sempre vale 3.
- Portão que não existe:
  - a entrada da receptora e do sensor é ignorada (tratada como comando ausente e portão fechado);
  - não roda `processaPortao()`, não gera evento, não arma sirene e não pulsa motor;
  - não entra no intertravamento;
  - `open_or_close_gate`, `fotocelula` e `maintenance_port` desse número respondem `failed`;
  - a fotocélula correspondente permanece em HIGH.
- Placa de 1 portão não tem intertravamento. Placa de 2 portões só intertrava 1 e 2.

## Testes
`teste_portoes_da_placa_e_rele_alarme` (em `teste_completo()`): leitura do serial, placa de 1 e de 2 portões, portão em manutenção fora do intertravamento e relé 8 aberto com a sirene ligada.
