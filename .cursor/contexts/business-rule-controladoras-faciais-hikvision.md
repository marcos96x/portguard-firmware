---
alwaysApply: false
---

# Regra de negócio: controladoras faciais Hikvision

Implementada em `script.cpp`.

## Descoberta na rede
- A cada `intervalo_controladoras_faciais_ms` (10 s), com serial da placa já gravado e com Wi-Fi ou cabo no ar, a placa sonda os dois meios que estiverem com IP.
- Cada sonda abre um UDP preso na interface daquele meio (`udp_bind_netif` + `udp_sendto_if` no IP de `WiFi.localIP()` ou `ETH.localIP()`). Wi-Fi e cabo na mesma faixa, como `192.168.15.0/24`, continuam sendo duas buscas: o pacote do cabo sai pela netif do cabo e não pela rota do Wi-Fi.
- A sonda é o SADP da Hikvision: UDP porta `37020`, para `239.255.255.250`, para o broadcast da rede da interface e para `255.255.255.255`, escutando por `tempo_escuta_sondagem_controladora_facial_ms` (1500 ms). O XML é `<Probe><Uuid>...</Uuid><Types>inquiry</Types></Probe>`. A resposta `ProbeMatch` volta em unicast para o IP e a porta de origem.
- O monitor serial mostra a interface, o IP, a máscara, o broadcast, se cada envio saiu e o IP de cada pacote recebido.
- Ao fim da rodada, mesmo sem leitor novo, a placa envia o log `facial_search` em `/plaque-events-logs/save`. O `event_data` é `{"placas":[...]}` com `serial`, `ip`, `porta`, `modelo`, `firmware` e `meio` (`wifi` ou `ethernet`) de cada controladora vista nesta rodada. Lista vazia vai `{"placas":[]}`. O mesmo serial não repete.
- Entra na lista o leitor cujo serial, modelo ou tipo é terminal facial: `DS-K1T` (inclui `DS-K1T673`, exceto `DS-K1TA`), `DS-K56`, `MinMoe`, `Face` ou `Facial`. A tag pode vir com prefixo ou atributo. Câmera `DS-2` não entra. Tipo só numérico não impede o cadastro quando o serial já identifica o leitor.
- Do pacote saem `DeviceSN` (serial), `IPv4Address`, `HttpPort` (80 se vier vazio), `DeviceDescription` (modelo) e `SoftwareVersion` (firmware).

## Preferences
- A lista fica no namespace `portguard`, chave `faciais_rede` (12 caracteres; o NVS aceita até 15).
- Cada item é `{"serial","ip","porta","modelo","firmware","enviada","meio"}`. `meio` é `wifi` ou `ethernet`, a interface em que o leitor respondeu. A busca do sync é pelo `serial`. O HTTP da ISAPI sai pelo IP dessa interface, inclusive quando Wi-Fi e cabo estão no mesmo /24.
- No máximo `quantidade_maxima_controladoras_faciais` (8). Serial repetido não gera outro item. Se o IP mudar, o item é atualizado e `enviada` permanece.
- `enviada` `0` ainda não foi aceito em `/facials/discover`. `1` já foi. Falha da API deixa `0` para repetir no próximo ciclo.

## API Portguard
- Leitor novo (ou ainda não confirmado): `POST /api/v1/facials/discover` com `internal_token`, `serial` da placa, `facial_serial_id`, `facial_model_id` e `facial_firmware`.
- No mesmo ciclo de 10 s: `POST /api/v1/facials/syncs/list` com `internal_token`, `serial_id` da placa, `quantity` 5, `quantidade` 5 e `status` `pending`. A fila dessa placa vem em `list`.
- Para cada item de `list`: `user_id`, `name` (ou `user_name` se `name` vier vazio), `user_email`, `image_base64` e `facial_serial_id`. O objeto pode ter campos aninhados; o fim do item é a chave que fecha o objeto inteiro. O serial acha o IP na Preferences. Sem leitor local, sem foto ou com retorno de erro da facial, o id continua pendente.
- A resposta com a foto fica num bloco de memória (PSRAM quando existe). A `String` do núcleo não precisa segurar o base64 inteiro.
- Cada item aceito pela facial gera na hora `POST /api/v1/facials/syncs/update` com `ids` desse item. A rota marca o id como sincronizado. Id que a facial recusou não entra nesse POST.
- O monitor serial abre um bloco `SYNC FACIAL` para cada item encontrado. As linhas dizem que o sync foi recebido, em qual facial (serial e IP) a tentativa vai ocorrer e qual foi o retorno da facial.
- Erro da facial, leitor ainda não descoberto ou foto ausente gera o log `erro_sync` em `/plaque-events-logs/save`. O `event_data` é `{"id","user_id","facial_serial_id","erro"}` e `erro` é o texto devolvido pela facial.

## ISAPI Hikvision
- Antes de gravar, `POST /ISAPI/AccessControl/UserInfo/Search?format=json` procura o `employeeNo` (`user_id`). `numOfMatches` maior que zero, ou `employeeNoAlreadyExist`, significa que o usuário já está no leitor.
- Usuário novo: `POST /ISAPI/AccessControl/UserInfo/Record?format=json` com `employeeNo`, `name` e `email` (`user_email`). O nome vai em até 32 bytes.
- Usuário que já existe: a placa não reenvia o cadastro. Só a face muda.
- Foto, nos dois casos: a imagem chega em `image_base64` e é decodificada. O envio é `PUT /ISAPI/Intelligent/FDLib/FDSetUp?format=json`, multipart `FaceDataRecord` (`faceLibType` `blackFD`, `FDID` `1`, `FPID` = `employeeNo`) e `FaceImage`. Esse é o cadastro da Hikvision: cria a face ou substitui a que já está no leitor.
- Se o terminal recusar o `FDSetUp`, a mesma foto segue em `POST /ISAPI/Intelligent/FDLib/FaceDataRecord?format=json`, com o mesmo multipart. O retorno HTTP das duas tentativas aparece no monitor.
- A foto vem em `image_base64` (aceita data URI). Acima de 200 KB a Hikvision recusa e o sync fica pendente.
- Autenticação HTTP Digest. Usuário `admin`, senha `Portguard@123` (`usuario_controladora_facial_hikvision` e `senha_controladora_facial_hikvision`). O leitor precisa estar ativado com essa conta. O nonce sai de `GET /ISAPI/System/deviceInfo?format=json`.
- A requisição ISAPI sai pela interface em que o leitor foi descoberto. Com os dois meios no mesmo /24, o TCP é aberto já preso no IP dessa interface.

## Teste
`teste_controladoras_faciais_hikvision` (em `teste_completo()`): intervalo, chave, sonda DS-K1T, recusa de câmera, lista por serial, limite de 8, troca de IP sem reabrir descoberta já enviada, meio ethernet no mesmo /24 do Wi-Fi, corpos de discover/list/update, busca do usuário, Record com e-mail, item pending com nome, e-mail e serial, log `erro_sync`, FPID, `FDSetUp` e `FaceDataRecord`, Digest RFC 2617, base64, broadcast da interface e o JSON `facial_search` com leitor de Wi-Fi e de cabo na mesma rede. Não grava Preferences e não chama a rede.
`teste_heartbeat_ssid_e_sync_facial` (em `teste_completo()`): heartbeat com meio e SSID, e sync cujo JSON tem objeto interno ainda entrega serial, usuário e foto.
