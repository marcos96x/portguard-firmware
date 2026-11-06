# Log de abertura e fechamento do portão

O `gate_open` e o `gate_close` enviados em `/api/v1/plaque-events-logs/save` usam `event_data` com o portão.

Quando o comando que abriu ou fechou veio de um botão de controle vinculado, o mesmo log inclui o serial desse controle:

{"port":1, "button_id": "1214212121"}

- O `button_id` é o identificador decimal já gravado no botão.
- Ele vale para a abertura se o pulso foi de abertura e o sensor abriu dentro da autorização.
- Ele vale para o fechamento se o pulso foi de fechamento e o sensor fechou em seguida.
- Se a autorização de abertura expira sem o sensor abrir, o `button_id` não entra num `gate_open` posterior.
- Receptora da placa e abertura sem comando continuam com {"port":N}, sem `button_id` e sem `user_id`.
- O `open_or_close_gate` guarda o `user_id` da ação. O `gate_open` ou `gate_close` daquele comando sai como {"port":1, "user_id": "42"}.
- O `user_id` vale para a abertura se o comando foi de abertura e o sensor abriu dentro da autorização.
- O `user_id` vale para o fechamento se o comando foi de fechamento e o sensor fechou em seguida.
- Um pulso de controle ou da receptora no lugar desse comando não leva o `user_id`.
- Se a autorização de abertura expira sem o sensor abrir, o `user_id` não entra num `gate_open` posterior.
- Abertura bloqueada pelo intertravamento não guarda o `user_id`.
- `gate_broken`, `gate_not_closed` e os demais eventos não recebem esse campo.
- O log imediato do pulso em `/api/app/v1/plaques/logs` permanece como está.
