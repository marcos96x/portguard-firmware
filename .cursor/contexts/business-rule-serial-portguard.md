---
alwaysApply: false
---

# Regra de negócio: serial da placa em Preferences (`serial_portguard`)

Implementada em `script.cpp`.

## Regras
- O serial não fica mais compilado no firmware. A cópia em memória é `serial_placa`, carregada no `setup()` de Preferences, namespace `portguard`.
- A chave pedida `serial_portguard` tem 16 caracteres. O NVS da ESP32 aceita 15, então a chave gravada é `serial_portguar`. Sem esse ajuste o `putString` falha e a instalação nunca conclui.
- Com valor gravado, todas as requisições que identificam a placa (`serial` e `serial_id`) usam esse texto.
- Sem valor, a cada `intervalo_consulta_instalacao_placa_ms` (5 s) a placa consulta `POST /api/v1/plaque-install/waiting` com `internal_token` em formulário, no mesmo padrão das outras rotas `/api/v1`.
  - Sem internet, o monitor avisa e a tentativa recomeça em 5 s.
  - `{"list":[],"status":200}` significa que ainda não há serial. O monitor mostra o retorno e a frase de nova tentativa em 5 s.
  - `{"list":[{"serial":"1port-HQ3DJM9GN2BW"}],"status":200}` entrega o serial. O monitor mostra `Serial encontrado: ...` e o retorno.
- Ao encontrar o serial, a placa grava na Preferences (`Serial configurado em preferences: ...`) e confirma com `POST /api/v1/plaque-install/complete` no JSON `{"internal_token":"...","serial_id":"..."}`. Sucesso imprime `Instalação completa` e o retorno. Falha repete só a confirmação a cada 5 s.
- Enquanto `serial_portguard` estiver vazio, o `loop()` não consulta a fila, não envia heartbeat, eventos, cadastro de botão nem log de pulso. A rede continua subindo para a consulta de instalação existir. O controle local dos portões segue na tarefa própria.
- Depois que o serial está gravado, fila, heartbeat, logs e o restante da placa voltam ao comportamento normal.

## Testes
`teste_serial_portguard` (em `teste_completo()`): chave e intervalo, lista vazia, serial do exemplo, corpo do `/plaque-install/complete` e uso desse serial nas requisições. Não grava Preferences e não chama a API.
