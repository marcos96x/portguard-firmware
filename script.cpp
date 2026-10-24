/*
  ============================================================
  PORTGUARD - MAV1022 (ESP32-S3)
  Ethernet W5500 + Wi-Fi + Sensor + Sirene + API
  ============================================================

  PLACA:
  MAV1022, ESP32-S3. Entradas e saídas de campo passam pelos PCA9534
  no I2C (GPIO8 SDA, GPIO9 SCL). Não há GPIO direto de borne.

  ------------------------------------------------------------
  ENTRADAS (PCA9534 0x21, ativa em nível baixo)
  ------------------------------------------------------------
  ENTRADA 1 / canal 1 / bit 0 - receptora portão 1 (repouso inativo)
  ENTRADA 2 / canal 2 / bit 1 - sensor portão 1   (repouso ativo = fechado)
  ENTRADA 3 / canal 3 / bit 2 - receptora portão 2 (repouso inativo)
  ENTRADA 4 / canal 4 / bit 3 - sensor portão 2   (repouso ativo = fechado)
  ENTRADA 5 / canal 5 / bit 4 - receptora portão 3 (repouso inativo)
  ENTRADA 6 / canal 6 / bit 5 - sensor portão 3   (repouso ativo = fechado)

  Os três sensores usam a mesma regra: opto ativo (bit em nível baixo) = portão fechado.
  Abrir o sensor solta o opto, o bit sobe e o estado passa a aberto.

  ------------------------------------------------------------
  SAÍDAS (PCA9534 0x20, bit 1 = relé energizado)
  ------------------------------------------------------------
  SAÍDA 1 / canal 1 / bit 0 - motor portão 1        (repouso LOW)
  SAÍDA 2 / canal 2 / bit 1 - motor portão 2        (repouso LOW)
  SAÍDA 3 / canal 3 / bit 2 - motor portão 3        (repouso LOW)
  SAÍDA 4 / canal 4 / bit 3 - fotocélula portão 1  (repouso HIGH)
  SAÍDA 5 / canal 5 / bit 4 - fotocélula portão 2  (repouso HIGH)
  SAÍDA 6 / canal 6 / bit 5 - fotocélula portão 3  (repouso HIGH)
  SAÍDA 7 / canal 7 / bit 6 - sirene                (repouso LOW)
  SAÍDA 8 / canal 8 / bit 7 - central de alarme, inverso da sirene (repouso HIGH).
                       Quando a sirene dispara, o relé 8 desenergiza e o contato abre.

  Campo continua no PCA9534 (8 entradas e 8 saídas). GPIO do ESP32-S3 fica no
  I2C, Ethernet, USB, RTC e RF433; não há borne direto sobrando para o portão 3.

  ------------------------------------------------------------
  ETHERNET W5500 (manual MAV1022 revisão 1.1)
  ------------------------------------------------------------
  GPIO11 = MOSI
  GPIO12 = MISO
  GPIO13 = SCK
  GPIO14 = CS
  GPIO10 = INT
  GPIO7  = RESET

  GPIO48 = receptor RF433 (sinal invertido por Q2). Com o botao de gravar
           controle, a janela de 20 s imprime o quadro RF de cada tecla.
           Esse aviso nao aciona rele.
  GPIO5  = botao cadastrar controle (ativo em nivel baixo). A assinatura
           repetida entra em Preferences "buttons" com portao 0 e status livre
           e o loop envia JSON para /api/app/v1/plaque-buttons/save.
           Cadastrar de novo o mesmo botao volta para livre/portao 0 e envia
           /api/app/v1/plaque-buttons/update com control_id 0 e port_number 0.
           O pulso so abre o portao indicado se o status estiver vinculado.
           Controle gravado so na lista binaria antiga, sem entrada em buttons,
           continua abrindo o portao 1.
  GPIO19/GPIO20 = USB nativa (não usar)
  GPIO15/GPIO16 = cristal do RTC (não usar)
  GPIO3  = LED azul D6 (Wi-Fi): aceso com IP, apagado sem conexao
  GPIO21 = LED vermelho D7 (Ethernet): aceso com cabo e IP, apagado sem link
  GPIO47 = LED laranja D8 (status): 1 s aceso e 1 s apagado, ritmo de conexao com o servidor

  ------------------------------------------------------------
  FUNCIONAMENTO (portões 1, 2 e 3)
  ------------------------------------------------------------
  0. Entradas, regras e saídas rodam numa tarefa FreeRTOS própria
     (a cada 10 ms), independente das requisições HTTP do loop.
     Eventos vão para uma fila e são enviados à API com event_data {"port": N}.

  1. Comando na receptora do portão:
     - Motor do portão recebe pulso de 1 segundo.
     - Com o portão fechado, libera janela de 13 segundos para abertura.
       Repique do sensor enquanto o relé do motor está atracado não gasta essa autorização.
     - Intertravamento (intertravamento_ativo): comando de abertura é
       ignorado enquanto qualquer outro portão da placa estiver aberto.
       Placa de 1 portão não intertrava. Placa de 2 portões só olha 1 e 2.
       Portão em manutenção não entra na conta. Fechamento nunca é bloqueado.

  2. Sensor abre sem autorização:
     - Sirene contínua enquanto o portão estiver aberto.
     - Envia gate_broken para API (no máximo 1 a cada 60 segundos,
       contando a partir do último gate_broken enviado, de qualquer portão).

  3. Portão aberto por mais de 120 segundos (tempo_maximo_portao_aberto_ms):
     - Envia gate_timeout_open uma vez por abertura, com o título
       "Portão aberto por mais de 2 minutos" e event_data {"port": N}.
     - Sirene 1 segundo ligada / 2 segundos desligada até fechar.
     - O cronômetro é de cada portão e zera a cada abertura.

  4. Portão fecha:
     - Alarmes daquele portão são encerrados, exceto após abertura sem
       autorização: a sirene continua contínua por mais 120 segundos.

  SAÍDA 8 é sempre o inverso da sirene (SAÍDA 7). SAÍDAS 4, 5 e 6 mantêm as
  fotocélulas dos portões 1, 2 e 3 alimentadas (HIGH), exceto quando a ação
  fotocelula da API desliga a do portão indicado (LOW).

  5. Rede:
     - Com Wi-Fi conectado, as requisições usam Wi-Fi.
     - Sem Wi-Fi persistido, o cabo com IP assume a comunicação com o aplicativo:
       as requisições saem pela Ethernet e a config_plaque pendente é confirmada
       (completed), para a placa não permanecer em aguardando configuração.
     - Se o Wi-Fi já estava persistido e não conectar, o cabo segue como reserva.
     - Sem os dois, o Serial explica o que falhou em cada meio.

  6. API:
     - Consulta da fila (/plaque-queues-actions/list) a cada meio segundo
       com rede disponível (5 s de espera após uma falha); sem rede,
       tenta a cada 30 segundos.
     - Heartbeat a cada 30 segundos quando houver link.
     - config_plaque: aplica o Wi-Fi recebido em action_data. Sem Wi-Fi persistido
       e com o cabo no ar, a configuração é confirmada pelo cabo.
     - reset_plaque: apaga o Wi-Fi e usa cabo até um novo config_plaque.
     - open_or_close_gate: action_data vazio ou 1 = portão 1, 2 = portão 2, 3 = portão 3.
       Mesmo efeito da receptora (abre se fechado, fecha se aberto),
       respeitando o intertravamento.
     - down_siren: desliga a sirene (encerra os alarmes ativos dos três portões).
     - open_or_close_gate e down_siren criados há mais de 60 s (pelo relógio
       NTP) não são executados e respondem failed.
     - intertravamento: action_data 0 desativa, 1 ativa (persistido).
     - fotocelula_1 / fotocelula_2 / fotocelula_3: action_data 0 desliga (LOW) e 1 liga (HIGH)
       a fotocélula do portão 1 (SAÍDA 4) / portão 2 (SAÍDA 5) / portão 3 (SAÍDA 6) (persistido).
       fotocelula (sem número) aceita {"port":N,"value":V} ou só 0/1 (portão 1).
     - Toda ação lida é respondida (completed/failed) para a fila andar; se a
       API continuar devolvendo uma ação já tratada, o status é reenviado.
     - maintenance_port_1 / _2 / _3: action_data 0 tira e 1 coloca aquele portão
       em manutenção (persistido). set_maintenance / unset_maintenance aplicam
       o mesmo a todos os portões que a placa tem. Em manutenção aquele portão
       não toca sirene, não arma alarme e não envia evento. Os outros portões
       e o heartbeat seguem normais. A fila continua sendo consultada.
     - update_button_control: action_data traz button_id, port_number e status
       (um objeto ou varios). Atualiza cada botao no indice Preferences "buttons",
       confere a gravacao e so entao responde a fila. O pulso do botao passa pelo
       intertravamento.
     - Cadastro de botao envia JSON para /api/app/v1/plaque-buttons/save.
       Re-cadastro volta o botao para livre/portao 0 e envia
       /api/app/v1/plaque-buttons/update com control_id 0 e port_number 0.
     - Pulso de controle vinculado envia abertura/fechamento para
       /api/app/v1/plaques/logs com button_id e port_number.
  ============================================================
*/

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Network.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <ETH.h>
#include <SPI.h>
#include <Wire.h>

// ============================================================
// IDENTIFICAÇÃO E API
// ============================================================

const String plaque_serial = "1port-7PWV7S9536CF";

// Quantidade de portões ligados nesta placa. O serial (1port-...) só identifica a placa na API
// e não pode reduzir isso: com 1 a entrada 6 e a saída 3 ficam mortas.
// Placa de 1 portão usa 1; placa de 2 portões usa 2; esta placa está cabeada para 3.
const uint8_t quantidade_portoes_configurada = 3;
uint8_t quantidade_portoes_placa = quantidade_portoes_configurada;
const String api_url = "https://portguard.milesmp.com.br/api/v1";
const String api_url_aplicativo = "https://portguard.milesmp.com.br/api/app/v1";
const String api_token = "bananapratadoisreaisoquilo";
const char rota_cadastro_botao_controle[] = "/plaque-buttons/save";
const char rota_atualizacao_botao_controle[] = "/plaque-buttons/update";
const char rota_log_pulso_botao_controle[] = "/plaques/logs";
const char tipo_conteudo_json[] = "application/json";
const char operacao_api_botao_cadastrar = 'c';
const char operacao_api_botao_resetar = 'r';

// ============================================================
// WI-FI (cache em RAM — persistência em Preferences)
// ============================================================

bool setup_finished = false;
String wifi_ssid = "";
String wifi_password = "";
bool wifiCredenciaisPendentes = false;
bool wifiConfigFalhou = false;
bool wifi_persistido_ja_iniciado = false;
long queueIdPendente = 0;
long ultimoQueueIdNotificado = 0;
unsigned long momentoInicioConfigWifi = 0;
const unsigned long TIMEOUT_CONFIG_WIFI_MS = 10000;
String wifiSsidRejeitado = "";
String wifiPasswordRejeitado = "";

Preferences preferences;
NetworkClientSecure httpsClient;

// ============================================================
// ETHERNET W5500 - MAV1022 / ESP32-S3
// ============================================================

#define ETH_PHY_TYPE ETH_PHY_W5500
#define ETH_PHY_ADDR 1
#define ETH_PHY_CS 14
#define ETH_PHY_IRQ 10
#define ETH_PHY_RST 7

#define ETH_SPI_SCK 13
#define ETH_SPI_MISO 12
#define ETH_SPI_MOSI 11

bool ethIniciado = false;
bool ethBeginOk = false;
bool ethComIp = false;
bool ethernetDetectado = false;
bool ethFlagPendenteSalvar = false;

// ============================================================
// INTERVALOS E TEMPORIZADORES DE REDE
// ============================================================

const unsigned long INTERVALO_API_MS = 30000;
const unsigned long INTERVALO_VALIDAR_WIFI_MS = 1000;
const unsigned long INTERVALO_RECONECTAR_WIFI_MS = 5000;
const unsigned long INTERVALO_ERRO_REDE_MS = 10000;

unsigned long ultimoCicloApi = 0;
unsigned long ultimoValidarWifi = 0;
unsigned long ultimaTentativaWifi = 0;
unsigned long ultimoErroRede = 0;
bool cicloApiAssimQueRedeSubir = true;

// Consulta da fila (/plaque-queues-actions/list) a cada meio segundo; após uma falha espera um pouco mais
// para não repetir o erro no log a cada consulta. O heartbeat continua em INTERVALO_API_MS.
const unsigned long intervalo_consulta_fila_ms = 500;
const unsigned long intervalo_espera_apos_falha_consulta_fila_ms = 5000;
unsigned long momento_ultima_consulta_fila = 0;
unsigned long momento_ultima_falha_consulta_fila = 0;
bool consulta_fila_falhou = false;
bool requisicao_sem_log_de_sucesso = false;
long ultimo_queue_id_exibido_no_log = 0;

// A API devolve a ação pendente mais antiga primeiro: uma ação sem resposta trava todas as seguintes.
// Guarda o último status respondido para reenviá-lo (sem executar a ação de novo) se a API continuar
// devolvendo a mesma ação como pendente, por exemplo após uma falha de rede no /plaque-queues-actions/save.
long queue_id_ultimo_status_respondido = 0;
String ultimo_status_respondido_fila = "";
unsigned long momento_ultimo_envio_status_fila = 0;
const unsigned long intervalo_reenvio_status_fila_ms = 5000;

// open_or_close_gate e down_siren criados há mais de validade_acao_imediata_fila_segundos não são executados,
// para que comandos acumulados na fila (placa desligada ou fila travada) não abram portões fora de hora.
// created_at vem no horário de Brasília (UTC-3); o relógio da placa é sincronizado por NTP em UTC.
const long long validade_acao_imediata_fila_segundos = 60;
const long long fuso_horario_servidor_segundos = -3 * 3600;
const long long epoch_minimo_relogio_sincronizado = 1700000000;
bool sincronizacao_relogio_iniciada = false;
// Ações imediatas lidas logo após a rede subir esperam o NTP até este limite; depois são executadas sem validar a idade.
unsigned long momento_inicio_sincronizacao_relogio = 0;
const unsigned long tempo_maximo_espera_sincronizacao_relogio_ms = 15000;

// ============================================================
// HARDWARE MAV1022 (.cursor/contexts/architecture.md)
// ============================================================

const uint8_t pino_i2c_sda = 8;
const uint8_t pino_i2c_scl = 9;
const uint8_t endereco_pca9534_saidas = 0x20;
const uint8_t endereco_pca9534_entradas = 0x21;
const uint32_t frequencia_i2c_hz = 100000;
const uint8_t registrador_entrada_pca9534 = 0x00;
const uint8_t registrador_saida_pca9534 = 0x01;
const uint8_t registrador_polaridade_pca9534 = 0x02;
const uint8_t registrador_configuracao_pca9534 = 0x03;

// Canal da placa N = bit (N - 1) do PCA9534.
const uint8_t canal_entrada_receptora_portao_1 = 1;
const uint8_t canal_entrada_sensor_portao_1 = 2;
const uint8_t canal_entrada_receptora_portao_2 = 3;
const uint8_t canal_entrada_sensor_portao_2 = 4;
const uint8_t canal_entrada_receptora_portao_3 = 5;
const uint8_t canal_entrada_sensor_portao_3 = 6;
const uint8_t canal_saida_motor_portao_1 = 1;
const uint8_t canal_saida_motor_portao_2 = 2;
const uint8_t canal_saida_motor_portao_3 = 3;
const uint8_t canal_saida_fotocelula_portao_1 = 4;
const uint8_t canal_saida_fotocelula_portao_2 = 5;
const uint8_t canal_saida_fotocelula_portao_3 = 6;
const uint8_t canal_saida_sirene = 7;
const uint8_t canal_saida_central_alarme = 8;

const uint8_t pino_receptor_rf433 = 48;
const uint8_t pino_botao_cadastrar_controle = 5;
// LEDs da MAV1022 (manual revisão 1.1). Nível alto acende o LED.
const uint8_t pino_led_wifi = 3;
const uint8_t pino_led_ethernet = 21;
const uint8_t pino_led_status_servidor = 47;
const unsigned long tempo_piscada_led_status_servidor_ms = 1000;
const unsigned long tempo_espera_cadastro_controle_rf433_ms = 20000;
const uint8_t quantidade_maxima_controles_rf433 = 8;
const unsigned long silencio_fim_quadro_rf433_us = 5000;
const unsigned long intervalo_minimo_comando_rf433_ms = 1000;
const int quantidade_maxima_pulsos_rf433 = 96;
const int quantidade_minima_pulsos_quadro_rf433 = 16;
const int bits_minimos_quadro_rf433 = 8;
const int bits_maximos_quadro_rf433 = 32;
const unsigned long intervalo_minimo_pulso_util_rf433_us = 250;
const unsigned long intervalo_maximo_pulso_util_rf433_us = 1500;
const int quantidade_minima_intervalos_assinatura_rf433 = 50;
const int quantidade_maxima_intervalos_assinatura_rf433 = 70;
const unsigned long intervalo_minimo_mensagem_assinatura_rf433_ms = 300;
const uint16_t duracao_pulso_curto_minima_rf433_us = 350;
const uint16_t duracao_pulso_curto_maxima_rf433_us = 650;
const uint16_t duracao_pulso_longo_minima_rf433_us = 800;
const uint16_t duracao_pulso_longo_maxima_rf433_us = 1150;
const uint32_t base_assinatura_fnv_rf433 = 2166136261UL;
const uint32_t primo_assinatura_fnv_rf433 = 16777619UL;
const uint32_t assinatura_botao_1_controle_rf433 = 0xFE280B33UL;
const uint32_t assinatura_botao_2_controle_rf433 = 0xE5B7B297UL;
const uint32_t assinatura_botao_3_controle_rf433 = 0x6958C26BUL;

// Cadastro dos botões de controle no índice "buttons" da Preferences (array JSON).
const char chave_armazenamento_botoes_controle[] = "buttons";
const uint8_t quantidade_maxima_botoes_controle = 20;
const uint8_t tamanho_identificador_botao_controle = 24;
const uint8_t tamanho_numero_portao_botao_controle = 4;
const uint8_t tamanho_status_botao_controle = 16;
const uint16_t tamanho_buffer_json_botoes_controle = 2560;
const unsigned long intervalo_reenvio_cadastro_botao_controle_ms = 5000;
const unsigned long intervalo_nova_tentativa_gravacao_botoes_controle_ms = 1000;
const uint8_t limite_tentativas_gravacao_botao_controle = 5;
const uint8_t tamanho_fila_logs_pulsos_botoes_controle = 8;

// Bits 0, 2 e 4 em alto: receptoras inativas. Bits 1, 3 e 5 em baixo: sensores ativos (portões fechados).
const uint8_t byte_entradas_repouso_seguro_pca9534 = 0x15;

// ============================================================
// TEMPOS DA LÓGICA DOS PORTÕES
// ============================================================

const unsigned long tempo_pulso_motor_ms = 1000;
// 8 s originais mais 5 s: o sensor do portão comandado pode demorar a abrir sem virar arrombamento.
const unsigned long acrescimo_janela_abertura_comando_ms = 5000;
const unsigned long janela_comando_autorizado_ms = 8000 + acrescimo_janela_abertura_comando_ms;
const unsigned long tempo_sirene_apos_fechamento_abertura_indevida_ms = 120000;
const unsigned long tempo_maximo_portao_aberto_ms = 120000;
const char titulo_aviso_portao_aberto_sem_fechar[] = "Portão aberto por mais de 2 minutos";
const char tipo_evento_portao_aberto_sem_fechar[] = "gate_timeout_open";
const unsigned long tempo_sirene_ligada_alerta_ms = 1000;
const unsigned long tempo_sirene_desligada_alerta_ms = 2000;
const unsigned long tempo_estabilizacao_entrada_ms = 50;
const unsigned long intervalo_tarefa_portoes_ms = 10;
const int limite_possivel_falha_sensor = 6;

// ============================================================
// ESTADO DOS PORTÕES
// ============================================================

struct EntradaEstabilizada {
  bool leitura_anterior;
  bool valor_estavel;
  unsigned long momento_ultima_mudanca;
};

struct EstadoPortao {
  uint8_t numero_portao;
  bool modo_teste;
  String eventos_registrados_teste;
  bool receptora_anterior;
  bool sensor_fechado_anterior;
  bool motor_acionado;
  unsigned long momento_inicio_pulso_motor;
  bool abertura_autorizada;
  unsigned long momento_autorizacao;
  bool portao_aberto;
  unsigned long momento_abertura;
  bool alarme_abertura_indevida_ativo;
  bool sirene_apos_fechamento_indevido_ativa;
  unsigned long momento_fechamento_abertura_indevida;
  bool alerta_tempo_aberto_ativo;
  unsigned long momento_inicio_alerta_tempo_aberto;
  int contador_possivel_falha_sensor;
};

struct NiveisSaidas {
  uint8_t motor_portao_1;
  uint8_t motor_portao_2;
  uint8_t motor_portao_3;
  uint8_t sirene;
  uint8_t fotocelula_portao_1;
  uint8_t fotocelula_portao_2;
  uint8_t fotocelula_portao_3;
  uint8_t central_alarme;
};

struct EventoPendente {
  char tipo_evento[24];
  uint8_t numero_portao;
};

struct BotaoControleCadastrado {
  char identificador[tamanho_identificador_botao_controle];
  char numero_portao[tamanho_numero_portao_botao_controle];
  char status[tamanho_status_botao_controle];
  char operacao_api_pendente;
  bool ocupado;
};

struct ResultadoConsultaBotaoControle {
  bool cadastrado;
  bool pode_pulsar;
  uint8_t numero_portao_pulso;
  char identificador[tamanho_identificador_botao_controle];
  char numero_portao[tamanho_numero_portao_botao_controle];
  char status[tamanho_status_botao_controle];
  char operacao_api_pendente;
};

struct ResultadoAtualizacaoBotaoControle {
  bool dados_validos;
  bool localizado;
  bool atualizado_na_memoria;
  char identificador[tamanho_identificador_botao_controle];
  char numero_portao[tamanho_numero_portao_botao_controle];
  char status[tamanho_status_botao_controle];
};

struct LogPulsoBotaoControle {
  char identificador_botao[tamanho_identificador_botao_controle];
  char numero_portao[tamanho_numero_portao_botao_controle];
  char tipo_evento[16];
};

EstadoPortao estado_portao_1;
EstadoPortao estado_portao_2;
EstadoPortao estado_portao_3;
EntradaEstabilizada entrada_receptora_portao_1;
EntradaEstabilizada entrada_sensor_portao_1;
EntradaEstabilizada entrada_receptora_portao_2;
EntradaEstabilizada entrada_sensor_portao_2;
EntradaEstabilizada entrada_receptora_portao_3;
EntradaEstabilizada entrada_sensor_portao_3;

volatile bool fotocelula_portao_1_alimentada = true;
volatile bool fotocelula_portao_2_alimentada = true;
volatile bool fotocelula_portao_3_alimentada = true;
// Quando true, comando de abertura de um portão é ignorado enquanto o outro estiver aberto
// (comandos de fechamento nunca são bloqueados).
volatile bool intertravamento_ativo = true;
// Manutenção é por portão (maintenance_port_1, _2 e _3). O portão em manutenção não toca sirene,
// não arma alarme, não envia evento e não participa do intertravamento. Os outros seguem normais.
volatile bool manutencao_portao_1 = false;
volatile bool manutencao_portao_2 = false;
volatile bool manutencao_portao_3 = false;

uint8_t ultimo_byte_entradas_pca9534 = byte_entradas_repouso_seguro_pca9534;
bool leitura_entradas_pca9534_valida = false;

bool codigo_rf433_esta_gravado = false;
uint32_t codigo_rf433_gravado = 0;
uint8_t quantidade_bits_codigo_rf433_gravado = 0;
bool gravacao_codigo_rf433_pendente = false;
bool aguardando_repeticao_quadro_rf433 = false;
uint32_t codigo_rf433_candidato = 0;
uint8_t quantidade_bits_codigo_rf433_candidato = 0;
unsigned long momento_ultimo_comando_rf433_reconhecido = 0;
volatile bool comando_rf433_portao_1_pendente = false;
bool cadastro_controle_rf433_ativo = false;
unsigned long momento_inicio_cadastro_controle_rf433 = 0;
bool escuta_botao_cadastrar_controle = false;
uint8_t quantidade_controles_rf433_gravados = 0;
uint32_t codigos_controles_rf433[quantidade_maxima_controles_rf433];
uint8_t bits_controles_rf433[quantidade_maxima_controles_rf433];
bool quadro_rf433_injetado_pendente = false;
bool quadro_rf433_injetado_valido = false;
uint32_t codigo_rf433_injetado = 0;
uint8_t quantidade_bits_codigo_rf433_injetado = 0;
bool assinatura_rf433_injetada_pendente = false;
uint32_t assinatura_rf433_injetada = 0;
bool assinatura_quadro_rf433_disponivel = false;
uint32_t assinatura_quadro_rf433_capturada = 0;
bool assinatura_rf433_candidata_disponivel = false;
uint32_t assinatura_rf433_candidata = 0;
volatile uint8_t comando_botao_controle_portao_pendente = 0;
char identificador_comando_botao_controle_pendente[tamanho_identificador_botao_controle] = "";
char numero_portao_comando_botao_controle_pendente[tamanho_numero_portao_botao_controle] = "";

BotaoControleCadastrado botoes_controle[quantidade_maxima_botoes_controle];
uint8_t quantidade_botoes_controle = 0;
volatile bool gravacao_botoes_controle_pendente = false;
uint32_t geracao_botoes_controle = 0;
unsigned long momento_proxima_gravacao_botoes_controle = 0;
char buffer_json_botoes_controle[tamanho_buffer_json_botoes_controle];
unsigned long momento_proxima_tentativa_envio_cadastro_botao_controle = 0;
portMUX_TYPE protecao_botoes_controle = portMUX_INITIALIZER_UNLOCKED;

volatile uint16_t duracoes_pulsos_rf433[quantidade_maxima_pulsos_rf433];
volatile int quantidade_pulsos_capturados_rf433 = 0;
volatile uint32_t momento_ultima_borda_rf433_us = 0;
volatile bool quadro_rf433_pronto_para_decodificar = false;
volatile bool quadro_rf433_excedeu_pulsos = false;
volatile bool receptor_rf433_iniciado = false;
unsigned long momento_ultima_mensagem_assinatura_rf433 = 0;
uint32_t quantidade_quadros_assinatura_rf433 = 0;
uint32_t quantidade_quadros_assinatura_rf433_regulares = 0;
uint32_t quantidade_quadros_assinatura_rf433_rejeitados = 0;

// gate_broken é notificação crítica: depois de um envio com sucesso, novos gate_broken (de qualquer portão)
// dentro de intervalo_minimo_entre_gate_broken_ms não são enviados para a API.
const unsigned long intervalo_minimo_entre_gate_broken_ms = 60000;
bool gate_broken_ja_enviado = false;
unsigned long momento_ultimo_gate_broken_enviado = 0;

const int tamanho_fila_eventos = 20;
QueueHandle_t fila_eventos_pendentes = nullptr;
// Comandos da fila da API para a tarefa dos portões: 1, 2 ou 3 = open_or_close_gate do portão,
// comando_remoto_desligar_sirene = down_siren. A tarefa devolve se o comando foi executado.
const uint8_t comando_remoto_desligar_sirene = 4;
QueueHandle_t fila_comandos_remotos_portao = nullptr;
QueueHandle_t fila_resultados_comandos_remotos = nullptr;
QueueHandle_t fila_logs_pulsos_botoes_controle = nullptr;
// A tarefa dos portões não pode consultar Wi-Fi nem Ethernet: isso segura o cadeado da rede
// enquanto o loop espera o resultado e o comando de abertura é apagado antes do pulso.
const unsigned long tempo_espera_resultado_comando_remoto_ms = 1000;
volatile bool wifi_conectado_para_led = false;
volatile bool cabo_conectado_para_led = false;
TaskHandle_t tarefa_controle_portoes = nullptr;

void conectarWifi();
void iniciarEthernet();
void preferirCaboSeWifiCaiu();
bool deveManterReconexaoAutomaticaWifi();
void configuraEstabilidadeWifiEsp32S3();
void registraEventosWifiEsp32S3();
bool enviarEventoLog(const char *eventType, uint8_t numero_portao = 0);
bool atualizarStatusFila(long queueId, const char *actionStatus);
bool wifiEstaConfigurado();
bool wifiConectado();
bool caboConectado();
bool deveTentarWifiAntesDoCabo();
const char *statusDaConfiguracaoQuandoWifiNaoSobe(bool cabo_disponivel);
bool caboDeveAssumirConfiguracaoDoAplicativo(bool cabo_disponivel, bool wifi_esta_conectado,
                                             bool configuracao_wifi_persistida);
bool confirmaConfiguracaoPeloCabo(long queue_id);
bool executarPostHttp(const char *destino, const String &url, const String &body, int timeoutMs, String *payloadOut,
                      const char *tipo_conteudo = "application/x-www-form-urlencoded");
void falharConfigWifiRecebida();
void limparConfigWifi();
bool processarResetPlaque(long queueId);
void carregaBotoesControle();
bool adicionaBotaoControleLivre(uint32_t assinatura);
ResultadoConsultaBotaoControle consultaBotaoControle(uint32_t assinatura);
uint8_t consomeComandoBotaoControle(char *identificador_botao, char *numero_portao_texto);
void enfileiraLogPulsoBotaoControle(const char *identificador_botao, const char *numero_portao_texto, bool comando_de_abertura);
bool salvaBotoesControle();
void enviaCadastrosBotoesControlePendentes();
void enviaLogsPulsosBotoesControle();
bool processarAtualizacaoBotaoControle(long queue_id, const String &payload);

// ============================================================
// LOG
// ============================================================

// Exibe uma linha de log no Serial no padrão da placa.
void print_log(const String &mensagem) {
  Serial.println(mensagem);
}

// ============================================================
// REDE
// ============================================================

bool wifiEstaConfigurado() {
  return setup_finished && wifi_ssid.length() > 0 && wifi_password.length() > 0;
}

// Diz se o Wi-Fi está associado e com IP. Com o rádio desligado não consulta WiFi.status(),
// porque no ESP32-S3 isso sobe a interface Wi-Fi e tira a rota padrão do cabo.
bool wifiConectado() {
  wifi_mode_t modo_wifi = WiFi.getMode();
  if (modo_wifi == WIFI_MODE_NULL || modo_wifi == WIFI_OFF) {
    return false;
  }
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

bool caboConectado() {
  if (!ethBeginOk) {
    return false;
  }

  IPAddress ip = ETH.localIP();
  if (ip != IPAddress(0, 0, 0, 0)) {
    ethComIp = true;
    return true;
  }

  return ethComIp && ETH.linkUp();
}

// O Wi-Fi só é tentado antes do cabo quando já está no ar, tem credencial salva ou há config_plaque em andamento.
bool deveTentarWifiAntesDoCabo() {
  return wifiConectado() || wifiEstaConfigurado() || wifiCredenciaisPendentes;
}

// Sem o Wi-Fi no ar, o cabo com IP fecha a configuração como completed. Sem cabo, a ação falha.
const char *statusDaConfiguracaoQuandoWifiNaoSobe(bool cabo_disponivel) {
  return cabo_disponivel ? "completed" : "failed";
}

// O cabo só assume a configuração do aplicativo quando tem IP, o Wi-Fi não está conectado e não há Wi-Fi persistido.
bool caboDeveAssumirConfiguracaoDoAplicativo(bool cabo_disponivel, bool wifi_esta_conectado,
                                             bool configuracao_wifi_persistida) {
  return cabo_disponivel && !wifi_esta_conectado && !configuracao_wifi_persistida;
}

// Confirma na API a configuração pendente pelo cabo, para o aplicativo sair de aguardando configuração.
// Não altera o Wi-Fi já persistido. Devolve true quando o cabo assumiu esta ação.
bool confirmaConfiguracaoPeloCabo(long queue_id) {
  if (!caboDeveAssumirConfiguracaoDoAplicativo(caboConectado(), wifiConectado(), setup_finished)) {
    return false;
  }
  if (queue_id <= 0 || queue_id == ultimoQueueIdNotificado) {
    return queue_id > 0 && queue_id == ultimoQueueIdNotificado;
  }

  ultimoQueueIdNotificado = queue_id;
  atualizarStatusFila(queue_id, statusDaConfiguracaoQuandoWifiNaoSobe(true));
  print_log("Cabo de rede assumiu a comunicacao com o aplicativo. Configuracao confirmada sem Wi-Fi.");
  return true;
}

String redeAtual() {
  if (wifiConectado()) {
    return "WIFI";
  }

  if (caboConectado()) {
    return "CABO DE REDE";
  }

  return "SEM REDE";
}

bool redeProntaParaApi() {
  return wifiConectado() || caboConectado();
}

void reportarErroRede(const char *mensagem) {
  unsigned long agora = millis();
  if (ultimoErroRede != 0 && agora - ultimoErroRede < INTERVALO_ERRO_REDE_MS) {
    return;
  }

  ultimoErroRede = agora;
  Serial.println();
  Serial.print("ERRO: ");
  Serial.println(mensagem);
}

void aplicarRotaDeRede() {
  if (wifiConectado()) {
    configuraEstabilidadeWifiEsp32S3();
    Network.setDefaultInterface(WiFi.STA);
    return;
  }

  preferirCaboSeWifiCaiu();
}

void preferirCaboSeWifiCaiu() {
  if (wifiConectado()) {
    return;
  }

  if (!ethIniciado) {
    iniciarEthernet();
  }

  if (caboConectado()) {
    Network.setDefaultInterface(ETH);
  }
}

// ============================================================
// EVENTOS ETHERNET
// ============================================================

void onEthEvent(arduino_event_id_t event, arduino_event_info_t info) {
  (void)info;

  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println();
      Serial.println("[ETH] W5500 iniciado.");
      ETH.setHostname("portguard");
      break;

    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println();
      Serial.println("[ETH] Cabo de rede conectado.");
      Serial.println("[ETH] Aguardando DHCP...");
      break;

    case ARDUINO_EVENT_ETH_GOT_IP:
      ethComIp = true;

      // Preferences não deve ser escrita neste callback.
      if (!ethernetDetectado) {
        ethFlagPendenteSalvar = true;
      }

      if (!wifiConectado()) {
        Network.setDefaultInterface(ETH);
        if (!setup_finished) {
          print_log("Sem Wi-Fi configurado. Cabo de rede assumiu a comunicacao com o aplicativo.");
        }
      }

      cicloApiAssimQueRedeSubir = true;

      Serial.println();
      Serial.println("======================================");
      Serial.println("CABO DE REDE CONECTADO COM SUCESSO");
      Serial.println("======================================");
      Serial.print("IP: ");
      Serial.println(ETH.localIP());
      Serial.print("Gateway: ");
      Serial.println(ETH.gatewayIP());
      Serial.print("Mascara: ");
      Serial.println(ETH.subnetMask());
      Serial.print("DNS: ");
      Serial.println(ETH.dnsIP());
      Serial.print("MAC: ");
      Serial.println(ETH.macAddress());
      Serial.print("Velocidade: ");
      Serial.print(ETH.linkSpeed());
      Serial.println(" Mbps");
      Serial.println("======================================");
      break;

    case ARDUINO_EVENT_ETH_LOST_IP:
      ethComIp = false;
      Serial.println();
      Serial.println("[ETH] IP perdido.");
      if (!wifiConectado()) {
        reportarErroRede("nao ha conexao com a internet.");
      }
      break;

    case ARDUINO_EVENT_ETH_DISCONNECTED:
      ethComIp = false;
      Serial.println();
      Serial.println("[ETH] Cabo desconectado.");
      if (!wifiConectado()) {
        reportarErroRede("nao ha conexao com a internet.");
      }
      break;

    case ARDUINO_EVENT_ETH_STOP:
      ethComIp = false;
      Serial.println();
      Serial.println("[ETH] Ethernet parado.");
      if (!wifiConectado()) {
        reportarErroRede("nao ha conexao com a internet.");
      }
      break;

    default:
      break;
  }
}

// ============================================================
// DIAGNÓSTICO E INÍCIO ETHERNET
// ============================================================

void diagnosticarEthernet() {
  Serial.println();
  Serial.println("-------- DEBUG ETHERNET --------");
  Serial.print("W5500 iniciado: ");
  Serial.println(ethBeginOk ? "SIM" : "NAO");
  Serial.print("Link fisico: ");
  Serial.println(ETH.linkUp() ? "SIM" : "NAO");
  Serial.print("DHCP/IP: ");
  Serial.println(ethComIp ? "SIM" : "NAO");

  if (ethBeginOk) {
    Serial.print("IP: ");
    Serial.println(ETH.localIP());
    Serial.print("Gateway: ");
    Serial.println(ETH.gatewayIP());
    Serial.print("Mascara: ");
    Serial.println(ETH.subnetMask());
    Serial.print("DNS: ");
    Serial.println(ETH.dnsIP());
    Serial.print("MAC: ");
    Serial.println(ETH.macAddress());

    if (ETH.linkUp()) {
      Serial.print("Velocidade: ");
      Serial.print(ETH.linkSpeed());
      Serial.println(" Mbps");
    }
  }

  Serial.print("Rede atual: ");
  Serial.println(redeAtual());
  Serial.print("Heap livre: ");
  Serial.println(ESP.getFreeHeap());
  Serial.println("--------------------------------");
}

void iniciarEthernet() {
  if (ethIniciado) {
    return;
  }

  ethIniciado = true;

  Serial.println();
  Serial.println("======================================");
  Serial.println("INICIANDO ETHERNET W5500");
  Serial.println("======================================");
  Serial.println("MAV1022 / ESP32-S3");
  Serial.println("SPI:");
  Serial.println("SCK  = GPIO13");
  Serial.println("MISO = GPIO12");
  Serial.println("MOSI = GPIO11");
  Serial.println("CS   = GPIO14");
  Serial.println("INT  = GPIO10");
  Serial.println("RST  = GPIO7");

  Network.onEvent(onEthEvent);
  SPI.begin(ETH_SPI_SCK, ETH_SPI_MISO, ETH_SPI_MOSI);

  ethBeginOk = ETH.begin(
      ETH_PHY_TYPE,
      ETH_PHY_ADDR,
      ETH_PHY_CS,
      ETH_PHY_IRQ,
      ETH_PHY_RST,
      SPI
  );

  Serial.println();
  if (ethBeginOk) {
    Serial.println("W5500 inicializado.");
    Serial.println("Aguardando cabo e DHCP...");
  } else {
    Serial.println("ERRO: W5500 nao iniciou.");
  }
  Serial.println("======================================");
}

// ============================================================
// PORTÕES: ENTRADAS, REGRAS E SAÍDAS
// ============================================================

// Lê um registrador do PCA9534 com STOP antes do requestFrom. No ESP32-S3 o repeated-start do Wire falha.
bool leRegistradorPca9534ComSucesso(uint8_t endereco, uint8_t registrador, uint8_t &valor) {
  for (int tentativa = 0; tentativa < 2; tentativa++) {
    Wire.beginTransmission(endereco);
    Wire.write(registrador);
    if (Wire.endTransmission(true) != 0) {
      continue;
    }
    if (Wire.requestFrom((uint8_t)endereco, (uint8_t)1) != 1) {
      continue;
    }
    if (!Wire.available()) {
      continue;
    }
    valor = (uint8_t)Wire.read();
    return true;
  }
  return false;
}

// Grava um registrador do PCA9534 e devolve true quando o expansor confirma o ACK.
bool escreveRegistradorPca9534(uint8_t endereco, uint8_t registrador, uint8_t valor) {
  Wire.beginTransmission(endereco);
  Wire.write(registrador);
  Wire.write(valor);
  return Wire.endTransmission(true) == 0;
}

// Entrada optoacoplada ativa deixa o bit do PCA9534 em nível baixo.
bool canalEntradaAtivo(uint8_t byte_entradas, uint8_t canal) {
  uint8_t mascara = (uint8_t)(1 << (canal - 1));
  return (byte_entradas & mascara) == 0;
}

// Lê o PCA9534 de entradas e registra mudança das ENTRADAS 1 a 4. Devolve false se o I2C falhar.
bool leEntradasPca9534(uint8_t &byte_entradas) {
  uint8_t lido = 0;
  if (!leRegistradorPca9534ComSucesso(endereco_pca9534_entradas, registrador_entrada_pca9534, lido)) {
    return false;
  }

  if (leitura_entradas_pca9534_valida && ((lido ^ ultimo_byte_entradas_pca9534) & 0x3F) != 0) {
    const char *nomes_entradas[] = {"receptora portao 1", "sensor portao 1", "receptora portao 2", "sensor portao 2",
                                    "receptora portao 3", "sensor portao 3"};
    print_log("PCA9534 entradas = 0x" + String(lido, HEX));
    for (uint8_t canal = 1; canal <= 6; canal++) {
      bool estava_ativa = canalEntradaAtivo(ultimo_byte_entradas_pca9534, canal);
      bool esta_ativa = canalEntradaAtivo(lido, canal);
      if (estava_ativa != esta_ativa) {
        String descricao = "ENTRADA " + String(canal) + " (" + nomes_entradas[canal - 1] + ") " +
                           (esta_ativa ? "ativa" : "inativa");
        if (canal == canal_entrada_sensor_portao_1 || canal == canal_entrada_sensor_portao_2 ||
            canal == canal_entrada_sensor_portao_3) {
          descricao += esta_ativa ? " (portao fechado)." : " (portao aberto).";
        } else {
          descricao += ".";
        }
        print_log(descricao);
      }
    }
  }

  ultimo_byte_entradas_pca9534 = lido;
  leitura_entradas_pca9534_valida = true;
  byte_entradas = lido;
  return true;
}

// Converte a leitura do PCA9534 nos quatro níveis lógicos usados pela regra dos portões.
void leEntradasDeCampo(bool &receptora_portao_1_acionada, bool &sensor_portao_1_fechado,
                       bool &receptora_portao_2_acionada, bool &sensor_portao_2_fechado,
                       bool &receptora_portao_3_acionada, bool &sensor_portao_3_fechado) {
  uint8_t byte_entradas = leitura_entradas_pca9534_valida ? ultimo_byte_entradas_pca9534
                                                          : byte_entradas_repouso_seguro_pca9534;
  leEntradasPca9534(byte_entradas);
  receptora_portao_1_acionada = canalEntradaAtivo(byte_entradas, canal_entrada_receptora_portao_1);
  sensor_portao_1_fechado = canalEntradaAtivo(byte_entradas, canal_entrada_sensor_portao_1);
  receptora_portao_2_acionada = canalEntradaAtivo(byte_entradas, canal_entrada_receptora_portao_2);
  sensor_portao_2_fechado = canalEntradaAtivo(byte_entradas, canal_entrada_sensor_portao_2);
  receptora_portao_3_acionada = canalEntradaAtivo(byte_entradas, canal_entrada_receptora_portao_3);
  sensor_portao_3_fechado = canalEntradaAtivo(byte_entradas, canal_entrada_sensor_portao_3);
}

// Empacota os seis níveis lógicos no byte do PCA9534 de saídas. Bit 1 energiza o relé.
uint8_t montaByteSaidasPca9534(const NiveisSaidas &niveis) {
  uint8_t byte_saidas = 0;
  if (niveis.motor_portao_1 == HIGH) {
    byte_saidas |= (uint8_t)(1 << (canal_saida_motor_portao_1 - 1));
  }
  if (niveis.motor_portao_2 == HIGH) {
    byte_saidas |= (uint8_t)(1 << (canal_saida_motor_portao_2 - 1));
  }
  if (niveis.motor_portao_3 == HIGH) {
    byte_saidas |= (uint8_t)(1 << (canal_saida_motor_portao_3 - 1));
  }
  if (niveis.sirene == HIGH) {
    byte_saidas |= (uint8_t)(1 << (canal_saida_sirene - 1));
  }
  if (niveis.fotocelula_portao_1 == HIGH) {
    byte_saidas |= (uint8_t)(1 << (canal_saida_fotocelula_portao_1 - 1));
  }
  if (niveis.fotocelula_portao_2 == HIGH) {
    byte_saidas |= (uint8_t)(1 << (canal_saida_fotocelula_portao_2 - 1));
  }
  if (niveis.fotocelula_portao_3 == HIGH) {
    byte_saidas |= (uint8_t)(1 << (canal_saida_fotocelula_portao_3 - 1));
  }
  // Relé 8 é o inverso da sirene. Com a sirene disparada o bit 7 fica em 0: o relé desliga
  // e o contato da central abre. Em repouso o relé 8 fica energizado (contato fechado).
  if (niveis.sirene != HIGH && niveis.central_alarme == HIGH) {
    byte_saidas |= (uint8_t)(1 << (canal_saida_central_alarme - 1));
  }
  return byte_saidas;
}

// Só um rótulo curto ("1port", "2port", "3port") define a quantidade. O serial da API,
// por exemplo "1port-7PWV7S9536CF", identifica a placa e continua valendo 3 portões.
uint8_t quantidadePortoesDoSerial(const String &serial) {
  if (serial == "1port" || serial == "1port-") {
    return 1;
  }
  if (serial == "2port" || serial == "2port-") {
    return 2;
  }
  if (serial == "3port" || serial == "3port-") {
    return 3;
  }
  return 3;
}

// Diz se este número de portão existe nesta placa.
bool portaoExisteNaPlaca(uint8_t numero_portao) {
  return numero_portao >= 1 && numero_portao <= quantidade_portoes_placa;
}

// Diz se o portão está em manutenção. Portão que a placa não tem nunca está em manutenção.
bool portaoEmManutencao(uint8_t numero_portao) {
  if (!portaoExisteNaPlaca(numero_portao)) {
    return false;
  }
  if (numero_portao == 2) {
    return manutencao_portao_2;
  }
  if (numero_portao == 3) {
    return manutencao_portao_3;
  }
  return manutencao_portao_1;
}

// Diz se o sensor deste portão deve impedir a abertura de outro.
// Placa de 1 portão não intertrava. Portão inexistente, em manutenção ou fechado não bloqueia.
bool sensorBloqueiaIntertravamento(uint8_t numero_portao, bool sensor_fechado) {
  if (quantidade_portoes_placa < 2 || !portaoExisteNaPlaca(numero_portao) || portaoEmManutencao(numero_portao)) {
    return false;
  }
  return !sensor_fechado;
}

// Mantém o PCA9534 de entradas em alta impedância, com o latch em alto.
// Se o pino de um sensor (ENTRADA 2, 4 ou 6) virar saída em nível baixo, o bit fica preso em
// "fechado": o intertravamento não bloqueia os outros portões e o arrombamento não arma sirene nem API.
// Retorna false quando o expansor não confirma a configuração.
bool garanteConfiguracaoEntradasPca9534() {
  bool latch_ok = escreveRegistradorPca9534(endereco_pca9534_entradas, registrador_saida_pca9534, 0xFF);
  bool polaridade_ok = escreveRegistradorPca9534(endereco_pca9534_entradas, registrador_polaridade_pca9534, 0x00);
  bool configuracao_ok = escreveRegistradorPca9534(endereco_pca9534_entradas, registrador_configuracao_pca9534, 0xFF);

  uint8_t configuracao_lida = 0x00;
  uint8_t polaridade_lida = 0xFF;
  bool confirmou_configuracao = leRegistradorPca9534ComSucesso(endereco_pca9534_entradas, registrador_configuracao_pca9534,
                                                               configuracao_lida) &&
                                configuracao_lida == 0xFF;
  bool confirmou_polaridade = leRegistradorPca9534ComSucesso(endereco_pca9534_entradas, registrador_polaridade_pca9534,
                                                             polaridade_lida) &&
                              polaridade_lida == 0x00;

  bool configuracao_valida = latch_ok && polaridade_ok && configuracao_ok && confirmou_configuracao && confirmou_polaridade;
  static bool configuracao_valida_anterior = true;
  if (!configuracao_valida && configuracao_valida_anterior) {
    print_log("ERRO: PCA9534 de entradas nao confirmou os pinos como entrada (config 0x" +
              String(configuracao_lida, HEX) + ").");
  } else if (configuracao_valida && !configuracao_valida_anterior) {
    print_log("PCA9534 de entradas recuperado: pinos voltaram a ser entrada.");
  }
  configuracao_valida_anterior = configuracao_valida;
  return configuracao_valida;
}

// Sobe o I2C, programa os dois PCA9534 e deixa o GPIO48 pronto para o receptor RF433.
void configuraPinos() {
  Wire.begin(pino_i2c_sda, pino_i2c_scl, frequencia_i2c_hz);

  NiveisSaidas repouso;
  repouso.motor_portao_1 = LOW;
  repouso.motor_portao_2 = LOW;
  repouso.motor_portao_3 = LOW;
  repouso.sirene = LOW;
  repouso.fotocelula_portao_1 = HIGH;
  repouso.fotocelula_portao_2 = HIGH;
  repouso.fotocelula_portao_3 = HIGH;
  repouso.central_alarme = HIGH;
  uint8_t byte_repouso = montaByteSaidasPca9534(repouso);

  // O registrador de saída é gravado antes de virar saída, para os relés não ligarem todos no default 0xFF.
  bool saidas_prontas = escreveRegistradorPca9534(endereco_pca9534_saidas, registrador_saida_pca9534, byte_repouso);
  saidas_prontas = escreveRegistradorPca9534(endereco_pca9534_saidas, registrador_polaridade_pca9534, 0x00) && saidas_prontas;
  saidas_prontas = escreveRegistradorPca9534(endereco_pca9534_saidas, registrador_configuracao_pca9534, 0x00) && saidas_prontas;
  bool entradas_prontas = garanteConfiguracaoEntradasPca9534();

  uint8_t configuracao_lida = 0xFF;
  bool confirmou_saidas = leRegistradorPca9534ComSucesso(endereco_pca9534_saidas, registrador_configuracao_pca9534,
                                                         configuracao_lida) &&
                          configuracao_lida == 0x00;

  if (!saidas_prontas || !entradas_prontas || !confirmou_saidas) {
    print_log("ERRO: PCA9534 nao confirmou a configuracao no I2C (saidas 0x20 / entradas 0x21).");
  } else {
    print_log("PCA9534 pronto: saidas 0x20, entradas 0x21, I2C GPIO8/GPIO9.");
  }

  pinMode(pino_receptor_rf433, INPUT);
  pinMode(pino_botao_cadastrar_controle, INPUT_PULLUP);
  pinMode(pino_led_wifi, OUTPUT);
  pinMode(pino_led_ethernet, OUTPUT);
  pinMode(pino_led_status_servidor, OUTPUT);
  digitalWrite(pino_led_wifi, LOW);
  digitalWrite(pino_led_ethernet, LOW);
  digitalWrite(pino_led_status_servidor, LOW);
}

// Nível do LED de link: alto enquanto a interface tem conexão, baixo quando perde.
uint8_t nivelLedConexao(bool conectado) {
  return conectado ? HIGH : LOW;
}

// Nível do LED de status: 1 s aceso e 1 s apagado, o ritmo que mostra a placa em contato com o servidor.
uint8_t nivelLedStatusServidor(unsigned long agora) {
  bool aceso = (agora / tempo_piscada_led_status_servidor_ms) % 2 == 0;
  return aceso ? HIGH : LOW;
}

// Atualiza os três LEDs da placa com o estado já lido no loop. Não consulta Wi-Fi nem Ethernet:
// a tarefa dos portões chama esta função, e essa consulta travava o pulso de abertura vindo do aplicativo.
void atualizaLedsIndicadoresRede(unsigned long agora) {
  static uint8_t nivel_anterior_led_wifi = 0xFF;
  static uint8_t nivel_anterior_led_ethernet = 0xFF;
  static uint8_t nivel_anterior_led_status = 0xFF;

  uint8_t nivel_wifi = nivelLedConexao(wifi_conectado_para_led);
  uint8_t nivel_ethernet = nivelLedConexao(cabo_conectado_para_led);
  uint8_t nivel_status = nivelLedStatusServidor(agora);

  if (nivel_wifi != nivel_anterior_led_wifi) {
    bool mudou = nivel_anterior_led_wifi != 0xFF;
    nivel_anterior_led_wifi = nivel_wifi;
    digitalWrite(pino_led_wifi, nivel_wifi);
    if (mudou) {
      print_log(nivel_wifi == HIGH ? "LED Wi-Fi aceso: placa com conexao Wi-Fi."
                                   : "LED Wi-Fi apagado: conexao Wi-Fi perdida.");
    }
  }

  if (nivel_ethernet != nivel_anterior_led_ethernet) {
    bool mudou = nivel_anterior_led_ethernet != 0xFF;
    nivel_anterior_led_ethernet = nivel_ethernet;
    digitalWrite(pino_led_ethernet, nivel_ethernet);
    if (mudou) {
      print_log(nivel_ethernet == HIGH ? "LED Ethernet aceso: cabo com conexao."
                                       : "LED Ethernet apagado: cabo sem conexao.");
    }
  }

  if (nivel_status != nivel_anterior_led_status) {
    nivel_anterior_led_status = nivel_status;
    digitalWrite(pino_led_status_servidor, nivel_status);
  }
}

// Inicializa o filtro de estabilização de uma entrada com a leitura atual.
void iniciaEntradaEstabilizada(EntradaEstabilizada &entrada, bool leitura, unsigned long agora) {
  entrada.leitura_anterior = leitura;
  entrada.valor_estavel = leitura;
  entrada.momento_ultima_mudanca = agora;
}

// Filtra repiques: só aceita uma nova leitura depois dela ficar estável por tempo_estabilizacao_entrada_ms.
// Retorna o valor estável atual da entrada.
bool atualizaEntradaEstabilizada(EntradaEstabilizada &entrada, bool leitura, unsigned long agora) {
  if (leitura != entrada.leitura_anterior) {
    entrada.leitura_anterior = leitura;
    entrada.momento_ultima_mudanca = agora;
  } else if (leitura != entrada.valor_estavel &&
             agora - entrada.momento_ultima_mudanca >= tempo_estabilizacao_entrada_ms) {
    entrada.valor_estavel = leitura;
  }

  return entrada.valor_estavel;
}

// Inicializa o estado de um portão tomando as entradas atuais como referência (não gera eventos).
// Em modo_teste os eventos ficam em eventos_registrados_teste em vez de irem para a API.
void iniciaEstadoPortao(EstadoPortao &portao, uint8_t numero_portao, bool receptora_acionada, bool sensor_fechado, unsigned long agora, bool modo_teste) {
  portao.numero_portao = numero_portao;
  portao.modo_teste = modo_teste;
  portao.eventos_registrados_teste = "";
  portao.receptora_anterior = receptora_acionada;
  portao.sensor_fechado_anterior = sensor_fechado;
  portao.motor_acionado = false;
  portao.momento_inicio_pulso_motor = 0;
  portao.abertura_autorizada = false;
  portao.momento_autorizacao = 0;
  portao.portao_aberto = !sensor_fechado;
  portao.momento_abertura = agora;
  portao.alarme_abertura_indevida_ativo = false;
  portao.sirene_apos_fechamento_indevido_ativa = false;
  portao.momento_fechamento_abertura_indevida = 0;
  portao.alerta_tempo_aberto_ativo = false;
  portao.momento_inicio_alerta_tempo_aberto = 0;
  portao.contador_possivel_falha_sensor = 0;
}

// Registra um evento do portão: em modo de teste guarda em memória; senão coloca na fila
// que o loop de rede envia para a API (nunca bloqueia a tarefa dos portões).
void registraEventoPortao(EstadoPortao &portao, const char *tipo_evento) {
  if (portao.modo_teste) {
    portao.eventos_registrados_teste += String(tipo_evento) + ";";
    return;
  }

  if (portaoEmManutencao(portao.numero_portao)) {
    print_log("Portao " + String(portao.numero_portao) + " em manutencao: evento " + String(tipo_evento) +
              " nao sera enviado para a API.");
    return;
  }

  if (fila_eventos_pendentes == nullptr) {
    print_log("ERRO: fila de eventos nao criada. Evento descartado: " + String(tipo_evento));
    return;
  }

  EventoPendente evento;
  strncpy(evento.tipo_evento, tipo_evento, sizeof(evento.tipo_evento) - 1);
  evento.tipo_evento[sizeof(evento.tipo_evento) - 1] = '\0';
  evento.numero_portao = portao.numero_portao;

  if (xQueueSend(fila_eventos_pendentes, &evento, 0) != pdTRUE) {
    print_log("ERRO: fila de eventos cheia. Evento descartado: " + String(tipo_evento) +
              " (portao " + String(portao.numero_portao) + ")");
  }
}

// Aplica as regras de negócio de um portão a partir das entradas já estabilizadas:
// pulso do motor, janela de autorização, intertravamento, abertura indevida, tempo excessivo aberto e falha de sensor.
// outro_portao_aberto informa o sensor estabilizado do outro portão: com intertravamento_ativo, o comando
// de abertura é ignorado (sem pulso e sem autorização) enquanto o outro portão estiver aberto.
// comando_remoto = true equivale a uma borda de subida da receptora (open_or_close_gate vindo da API).
// modo_manutencao = true: abertura sem autorização não arma alarme nem gera gate_broken, alarmes já ativos são
// encerrados e a contagem de tempo aberto fica parada (recomeça do zero quando a manutenção termina).
// Só altera o EstadoPortao e registra eventos; quem escreve nos pinos é aplicaSaidas().
// Retorna false somente quando um comando recebido foi bloqueado pelo intertravamento.
bool processaPortao(EstadoPortao &portao, bool receptora_acionada, bool sensor_fechado, unsigned long agora,
                    bool outro_portao_aberto = false, bool comando_remoto = false, bool modo_manutencao = false) {
  String prefixo_log = String(portao.modo_teste ? "[TESTE PORTAO " : "[PORTAO ") + String(portao.numero_portao) + "] ";
  String origem_comando = comando_remoto ? " (via API)" : "";

  bool comando_recebido = (receptora_acionada && !portao.receptora_anterior) || comando_remoto;
  // O portão estava fechado no ciclo anterior: o comando é de abertura, mesmo que o sensor
  // já apareça aberto neste ciclo (o pulso do relé não pode transformar essa abertura em arrombamento).
  bool comando_de_abertura = comando_recebido && portao.sensor_fechado_anterior;
  bool comando_bloqueado = comando_de_abertura && intertravamento_ativo && outro_portao_aberto && !modo_manutencao;

  if (comando_bloqueado) {
    print_log(prefixo_log + "Comando de abertura" + origem_comando +
              " ignorado pelo intertravamento: o outro portao esta aberto.");
  } else if (comando_recebido) {
    portao.motor_acionado = true;
    portao.momento_inicio_pulso_motor = agora;

    if (comando_de_abertura) {
      portao.abertura_autorizada = true;
      portao.momento_autorizacao = agora;
      print_log(prefixo_log + "Comando autorizado recebido" + origem_comando + ". Pulso no motor e abertura liberada por " +
                String(janela_comando_autorizado_ms / 1000) + " s.");
    } else {
      print_log(prefixo_log + "Comando recebido com portao aberto" + origem_comando + ". Pulso no motor para fechamento.");
    }
  }
  portao.receptora_anterior = receptora_acionada;

  if (!sensor_fechado && portao.sensor_fechado_anterior) {
    portao.portao_aberto = true;
    portao.momento_abertura = agora;
    print_log(prefixo_log + "Sensor abriu: portao aberto.");
    registraEventoPortao(portao, "gate_open");

    bool autorizacao_valida = portao.abertura_autorizada &&
                              agora - portao.momento_autorizacao <= janela_comando_autorizado_ms;
    portao.abertura_autorizada = false;

    if (autorizacao_valida) {
      portao.contador_possivel_falha_sensor = 0;
      // Enquanto o relé do motor está atracado, um repique na entrada do sensor não gasta a autorização.
      // A abertura que continuar depois do pulso é que confirma o comando.
      if (portao.motor_acionado) {
        portao.abertura_autorizada = true;
      }
      print_log(prefixo_log + "Abertura autorizada.");
    } else if (modo_manutencao) {
      print_log(prefixo_log + "Modo manutencao: abertura sem autorizacao ignorada (sem alarme e sem gate_broken).");
    } else if (!portao.alarme_abertura_indevida_ativo) {
      portao.alarme_abertura_indevida_ativo = true;
      portao.sirene_apos_fechamento_indevido_ativa = false;
      print_log(prefixo_log + "ALERTA: ABERTURA NAO AUTORIZADA! Sirene continua enquanto o portao estiver aberto e por mais " +
                String(tempo_sirene_apos_fechamento_abertura_indevida_ms / 1000) + " s apos fechar.");
      registraEventoPortao(portao, "gate_broken");
    }
  }

  if (sensor_fechado && !portao.sensor_fechado_anterior) {
    portao.portao_aberto = false;
    bool autorizacao_acompanha_pulso = portao.abertura_autorizada && portao.motor_acionado &&
                                       portao.momento_autorizacao != 0 &&
                                       agora - portao.momento_autorizacao <= janela_comando_autorizado_ms;
    if (!autorizacao_acompanha_pulso) {
      portao.abertura_autorizada = false;
    }
    print_log(prefixo_log + "Sensor fechou: portao fechado.");
    registraEventoPortao(portao, "gate_close");

    if (portao.alarme_abertura_indevida_ativo) {
      portao.alarme_abertura_indevida_ativo = false;
      portao.sirene_apos_fechamento_indevido_ativa = true;
      portao.momento_fechamento_abertura_indevida = agora;
      print_log(prefixo_log + "Portao fechado apos abertura nao autorizada. Sirene continua por " +
                String(tempo_sirene_apos_fechamento_abertura_indevida_ms / 1000) + " s.");
    }

    if (portao.alerta_tempo_aberto_ativo) {
      portao.alerta_tempo_aberto_ativo = false;
      print_log(prefixo_log + "Alerta de portao aberto encerrado.");
      registraEventoPortao(portao, "gate_timeout_close");
    }
  }
  portao.sensor_fechado_anterior = sensor_fechado;

  if (portao.motor_acionado && agora - portao.momento_inicio_pulso_motor >= tempo_pulso_motor_ms) {
    portao.motor_acionado = false;
    if (!sensor_fechado && portao.abertura_autorizada) {
      portao.abertura_autorizada = false;
    }
    print_log(prefixo_log + "Pulso do motor encerrado.");
  }

  if (portao.abertura_autorizada && agora - portao.momento_autorizacao > janela_comando_autorizado_ms) {
    portao.abertura_autorizada = false;
    portao.contador_possivel_falha_sensor++;
    print_log(prefixo_log + "Autorizacao expirou sem abertura. Possivel falha no sensor: " +
              String(portao.contador_possivel_falha_sensor) + "/" + String(limite_possivel_falha_sensor));

    if (portao.contador_possivel_falha_sensor >= limite_possivel_falha_sensor) {
      print_log(prefixo_log + "Limite atingido. Falha de leitura no sensor de fim de curso do portao " +
                String(portao.numero_portao) + ".");
      registraEventoPortao(portao, "plaque_problem");
      portao.contador_possivel_falha_sensor = 0;
    }
  }

  if (modo_manutencao) {
    if (portao.alarme_abertura_indevida_ativo || portao.sirene_apos_fechamento_indevido_ativa ||
        portao.alerta_tempo_aberto_ativo) {
      print_log(prefixo_log + "Modo manutencao: alarmes do portao encerrados.");
    }
    portao.alarme_abertura_indevida_ativo = false;
    portao.sirene_apos_fechamento_indevido_ativa = false;
    portao.alerta_tempo_aberto_ativo = false;
    if (portao.portao_aberto) {
      portao.momento_abertura = agora;
    }
  }

  if (portao.sirene_apos_fechamento_indevido_ativa &&
      agora - portao.momento_fechamento_abertura_indevida >= tempo_sirene_apos_fechamento_abertura_indevida_ms) {
    portao.sirene_apos_fechamento_indevido_ativa = false;
    print_log(prefixo_log + "Tempo da sirene apos fechamento da abertura nao autorizada encerrado.");
  }

  if (portao.portao_aberto && !portao.alerta_tempo_aberto_ativo &&
      agora - portao.momento_abertura >= tempo_maximo_portao_aberto_ms) {
    portao.alerta_tempo_aberto_ativo = true;
    portao.momento_inicio_alerta_tempo_aberto = agora;
    print_log(prefixo_log + "ALERTA: PORTAO ABERTO POR MAIS DE " + String(tempo_maximo_portao_aberto_ms / 1000) +
              " s! Sirene 1 s ligada / 2 s desligada ate fechar.");
    registraEventoPortao(portao, tipo_evento_portao_aberto_sem_fechar);
  }

  return !comando_bloqueado;
}

// Calcula o nível de cada saída a partir do estado dos três portões, sem escrever nos pinos.
// Abertura indevida deixa a sirene contínua enquanto o portão estiver aberto e por
// tempo_sirene_apos_fechamento_abertura_indevida_ms após fechar; tempo excessivo aberto faz a sirene intermitente.
// Com modo_manutencao a sirene fica sempre desligada (e a SAÍDA 8, inversa, em HIGH).
// quantidade_portoes abaixo de 3 ignora o estado dos portões que a placa não tem.
// portao_3 nulo mantém o motor do portão 3 desligado (testes que só exercitam dois portões).
NiveisSaidas calculaSaidas(const EstadoPortao &portao_1, const EstadoPortao &portao_2, bool alimentar_fotocelula_portao_1,
                           bool alimentar_fotocelula_portao_2, unsigned long agora, bool modo_manutencao = false,
                           const EstadoPortao *portao_3 = nullptr, bool alimentar_fotocelula_portao_3 = true,
                           uint8_t quantidade_portoes = 3) {
  const unsigned long ciclo_sirene_alerta_ms = tempo_sirene_ligada_alerta_ms + tempo_sirene_desligada_alerta_ms;
  bool considerar_portao_2 = quantidade_portoes >= 2;
  bool considerar_portao_3 = quantidade_portoes >= 3 && portao_3 != nullptr;
  bool fotocelula_portao_2_da_placa = quantidade_portoes >= 2;
  bool fotocelula_portao_3_da_placa = quantidade_portoes >= 3;
  bool sirene_ligada = false;

  // Cada portão entra na sirene pelo próprio estado. O portão 3 não pode ficar de fora:
  // arrombamento nele liga a SAÍDA 7 e derruba a SAÍDA 8 junto com os outros portões.
  const EstadoPortao *portao_considerado_1 = &portao_1;
  const EstadoPortao *portao_considerado_2 = considerar_portao_2 ? &portao_2 : nullptr;
  const EstadoPortao *portao_considerado_3 = considerar_portao_3 ? portao_3 : nullptr;
  const EstadoPortao *portoes_considerados[] = {portao_considerado_1, portao_considerado_2, portao_considerado_3};
  for (int indice = 0; indice < 3; indice++) {
    const EstadoPortao *portao = portoes_considerados[indice];
    if (portao == nullptr) {
      continue;
    }
    if (portao->alarme_abertura_indevida_ativo || portao->sirene_apos_fechamento_indevido_ativa) {
      sirene_ligada = true;
    } else if (portao->alerta_tempo_aberto_ativo &&
               (agora - portao->momento_inicio_alerta_tempo_aberto) % ciclo_sirene_alerta_ms < tempo_sirene_ligada_alerta_ms) {
      sirene_ligada = true;
    }
  }
  if (modo_manutencao) {
    sirene_ligada = false;
  }

  NiveisSaidas niveis;
  niveis.motor_portao_1 = portao_1.motor_acionado ? HIGH : LOW;
  niveis.motor_portao_2 = considerar_portao_2 && portao_2.motor_acionado ? HIGH : LOW;
  niveis.motor_portao_3 = considerar_portao_3 && portao_3->motor_acionado ? HIGH : LOW;
  niveis.sirene = sirene_ligada ? HIGH : LOW;
  niveis.fotocelula_portao_1 = alimentar_fotocelula_portao_1 ? HIGH : LOW;
  niveis.fotocelula_portao_2 = !fotocelula_portao_2_da_placa || alimentar_fotocelula_portao_2 ? HIGH : LOW;
  niveis.fotocelula_portao_3 = !fotocelula_portao_3_da_placa || alimentar_fotocelula_portao_3 ? HIGH : LOW;
  niveis.central_alarme = sirene_ligada ? LOW : HIGH;
  return niveis;
}

// Único ponto que escreve nas saídas físicas. Regrava o byte inteiro a cada ciclo para recuperar glitch no PCA9534.
void aplicaSaidas(const NiveisSaidas &niveis) {
  static uint8_t ultimo_nivel_sirene = LOW;
  static bool escrita_saidas_falhou = false;

  NiveisSaidas niveis_aplicados = niveis;
  if (niveis_aplicados.sirene == HIGH) {
    niveis_aplicados.central_alarme = LOW;
  }
  uint8_t byte_saidas = montaByteSaidasPca9534(niveis_aplicados);
  bool gravou = escreveRegistradorPca9534(endereco_pca9534_saidas, registrador_saida_pca9534, byte_saidas);
  if (!gravou && !escrita_saidas_falhou) {
    print_log("ERRO: falha ao gravar as saidas no PCA9534 0x20.");
  }
  escrita_saidas_falhou = !gravou;

  if (niveis_aplicados.sirene != ultimo_nivel_sirene) {
    ultimo_nivel_sirene = niveis_aplicados.sirene;
    print_log(niveis_aplicados.sirene == HIGH ? "Sirene ligada (SAIDA 7 em HIGH). Rele 8 aberto (SAIDA 8 em LOW)."
                                             : "Sirene desligada (SAIDA 7 em LOW). Rele 8 fechado (SAIDA 8 em HIGH).");
  }

  static uint8_t ultimo_nivel_fotocelula_portao_3 = HIGH;
  if (niveis_aplicados.fotocelula_portao_3 != ultimo_nivel_fotocelula_portao_3) {
    ultimo_nivel_fotocelula_portao_3 = niveis_aplicados.fotocelula_portao_3;
    print_log(niveis_aplicados.fotocelula_portao_3 == HIGH ? "Fotocelula do portao 3 alimentada (SAIDA 6 em HIGH)."
                                                         : "Fotocelula do portao 3 desligada (SAIDA 6 em LOW).");
  }
}

// Desliga a sirene de um portão (ação down_siren): encerra o alarme de abertura indevida, a sirene pós-fechamento e o
// alerta de tempo aberto. Com o portão ainda aberto a contagem de tempo aberto recomeça, para o alerta não voltar
// no ciclo seguinte; uma nova abertura sem autorização volta a disparar a sirene normalmente.
void desligaSirenePortao(EstadoPortao &portao, unsigned long agora) {
  bool havia_alarme = portao.alarme_abertura_indevida_ativo || portao.sirene_apos_fechamento_indevido_ativa ||
                      portao.alerta_tempo_aberto_ativo;
  portao.alarme_abertura_indevida_ativo = false;
  portao.sirene_apos_fechamento_indevido_ativa = false;
  portao.alerta_tempo_aberto_ativo = false;
  if (portao.portao_aberto) {
    portao.momento_abertura = agora;
  }
  if (havia_alarme) {
    print_log(String(portao.modo_teste ? "[TESTE PORTAO " : "[PORTAO ") + String(portao.numero_portao) +
              "] Sirene desligada via API (down_siren): alarmes do portao encerrados.");
  }
}

// Liga ou desliga a alimentação da fotocélula de um portão (portão 1 = SAÍDA 4, portão 2 = SAÍDA 5, portão 3 = SAÍDA 6).
// Por padrão elas ficam alimentadas; desligar serve para permitir o fechamento quando a fotocélula estiver com defeito.
void defineAlimentacaoFotocelula(uint8_t numero_portao, bool alimentar) {
  String descricao_fotocelula = "Fotocelula do portao 1";
  String saida = "SAIDA 4";
  if (numero_portao == 3) {
    fotocelula_portao_3_alimentada = alimentar;
    descricao_fotocelula = "Fotocelula do portao 3";
    saida = "SAIDA 6";
  } else if (numero_portao == 2) {
    fotocelula_portao_2_alimentada = alimentar;
    descricao_fotocelula = "Fotocelula do portao 2";
    saida = "SAIDA 5";
  } else {
    fotocelula_portao_1_alimentada = alimentar;
  }
  if (alimentar) {
    print_log(descricao_fotocelula + " alimentada (" + saida + " em HIGH).");
  } else {
    print_log(descricao_fotocelula + " desligada (" + saida + " em LOW).");
  }
}

// Liga ou desliga o intertravamento (bloqueio de abertura de um portão enquanto qualquer outro está aberto).
void defineIntertravamento(bool ativo) {
  intertravamento_ativo = ativo;
  print_log(ativo ? "Intertravamento ativado." : "Intertravamento desativado.");
}

// Coloca ou tira um portão da manutenção. Portão fora da placa é ignorado.
void defineManutencaoPortao(uint8_t numero_portao, bool ativo) {
  if (!portaoExisteNaPlaca(numero_portao)) {
    print_log("Manutencao do portao " + String(numero_portao) + " ignorada: a placa nao tem esse portao.");
    return;
  }
  if (numero_portao == 2) {
    manutencao_portao_2 = ativo;
  } else if (numero_portao == 3) {
    manutencao_portao_3 = ativo;
  } else {
    manutencao_portao_1 = ativo;
  }
  print_log(String("Manutencao do portao ") + String(numero_portao) + (ativo ? " ATIVADA." : " DESATIVADA."));
}

// Classifica um intervalo do controle: S curto, L longo, ou ? fora da faixa usada na assinatura.
char classificaIntervaloPulsoRf433(uint16_t duracao_us) {
  if (duracao_us >= duracao_pulso_curto_minima_rf433_us && duracao_us <= duracao_pulso_curto_maxima_rf433_us) {
    return 'S';
  }
  if (duracao_us >= duracao_pulso_longo_minima_rf433_us && duracao_us <= duracao_pulso_longo_maxima_rf433_us) {
    return 'L';
  }
  return '?';
}

// Monta a assinatura FNV-1a da sequencia de simbolos. O mesmo quadro do controle gera sempre o mesmo codigo.
uint32_t calculaAssinaturaSimbolosRf433(const char *simbolos, uint8_t tamanho) {
  uint32_t assinatura = base_assinatura_fnv_rf433;
  assinatura ^= tamanho;
  assinatura *= primo_assinatura_fnv_rf433;
  for (uint8_t indice = 0; indice < tamanho; indice++) {
    assinatura ^= (uint8_t)simbolos[indice];
    assinatura *= primo_assinatura_fnv_rf433;
  }
  return assinatura;
}

// Traduz a assinatura para o botao conhecido do controle. Assinatura nova permanece apenas no monitor serial.
String identificaAssinaturaControleRf433(uint32_t assinatura) {
  if (assinatura == assinatura_botao_1_controle_rf433) {
    return ">>> BOTAO 1 IDENTIFICADO";
  }
  if (assinatura == assinatura_botao_2_controle_rf433) {
    return ">>> BOTAO 2 IDENTIFICADO";
  }
  if (assinatura == assinatura_botao_3_controle_rf433) {
    return ">>> BOTAO 3 IDENTIFICADO";
  }
  return ">>> ASSINATURA DESCONHECIDA";
}

// Aceita o quadro do controle e calcula a assinatura. Com publicar_no_monitor_serial, mostra o QUADRO RF
// e conta o quadro. Fora da janela de cadastro o calculo segue silencioso e nao aciona rele.
bool analisaQuadroAssinaturaRf433(const uint16_t *duracoes_us, int quantidade_intervalos, uint32_t &assinatura,
                                  bool publicar_no_monitor_serial = true) {
  if (publicar_no_monitor_serial) {
    quantidade_quadros_assinatura_rf433++;
  }
  if (quantidade_intervalos < quantidade_minima_intervalos_assinatura_rf433 ||
      quantidade_intervalos > quantidade_maxima_intervalos_assinatura_rf433) {
    if (publicar_no_monitor_serial) {
      quantidade_quadros_assinatura_rf433_rejeitados++;
    }
    return false;
  }

  char simbolos[quantidade_maxima_pulsos_rf433 + 1];
  int intervalos_regulares = 0;
  for (int indice = 0; indice < quantidade_intervalos; indice++) {
    simbolos[indice] = classificaIntervaloPulsoRf433(duracoes_us[indice]);
    if (simbolos[indice] != '?') {
      intervalos_regulares++;
    }
  }
  simbolos[quantidade_intervalos] = '\0';

  if ((uint32_t)intervalos_regulares * 100 < (uint32_t)quantidade_intervalos * 90) {
    if (publicar_no_monitor_serial) {
      quantidade_quadros_assinatura_rf433_rejeitados++;
    }
    return false;
  }

  assinatura = calculaAssinaturaSimbolosRf433(simbolos, (uint8_t)quantidade_intervalos);
  if (!publicar_no_monitor_serial) {
    return true;
  }

  quantidade_quadros_assinatura_rf433_regulares++;
  if (millis() - momento_ultima_mensagem_assinatura_rf433 < intervalo_minimo_mensagem_assinatura_rf433_ms) {
    return true;
  }

  momento_ultima_mensagem_assinatura_rf433 = millis();
  print_log("");
  print_log("==============================");
  print_log("QUADRO RF CAPTURADO!");
  print_log("Intervalos: " + String(quantidade_intervalos));
  print_log("Assinatura: 0x" + String(assinatura, HEX));
  print_log("Simbolos: " + String(simbolos));
  print_log(identificaAssinaturaControleRf433(assinatura));
  print_log("==============================");
  return true;
}

// Classifica um trem de pulsos curto/longo em um código binário. Ignora o silêncio de sincronismo e ruído sem par curto/longo.
bool decodificaTremDePulsosRf433(const uint16_t *duracoes_us, int quantidade_pulsos, uint32_t &codigo, uint8_t &quantidade_bits) {
  uint16_t duracoes_uteis[quantidade_maxima_pulsos_rf433];
  int quantidade_uteis = 0;
  for (int indice = 0; indice < quantidade_pulsos && quantidade_uteis < quantidade_maxima_pulsos_rf433; indice++) {
    if (duracoes_us[indice] >= 150 && duracoes_us[indice] <= 2500) {
      duracoes_uteis[quantidade_uteis++] = duracoes_us[indice];
    }
  }

  if (quantidade_uteis < quantidade_minima_pulsos_quadro_rf433 || (quantidade_uteis % 2) != 0) {
    return false;
  }

  uint32_t codigo_montado = 0;
  int bits = 0;
  for (int indice = 0; indice < quantidade_uteis; indice += 2) {
    uint16_t primeiro = duracoes_uteis[indice];
    uint16_t segundo = duracoes_uteis[indice + 1];
    uint16_t menor = primeiro < segundo ? primeiro : segundo;
    uint16_t maior = primeiro > segundo ? primeiro : segundo;
    if (menor == 0 || maior < (uint16_t)(menor * 2)) {
      return false;
    }
    codigo_montado = (codigo_montado << 1) | (primeiro > segundo ? 1u : 0u);
    bits++;
  }

  if (bits < bits_minimos_quadro_rf433 || bits > bits_maximos_quadro_rf433) {
    return false;
  }

  codigo = codigo_montado;
  quantidade_bits = (uint8_t)bits;
  return true;
}

// Copia o quadro fechado pela interrupção (ou pelo silêncio), calcula a assinatura e tenta decodificá-lo.
// A assinatura fica disponível mesmo quando o código binário não fecha, para o cadastro dos botões.
bool consomeQuadroCapturadoRf433(uint32_t &codigo, uint8_t &quantidade_bits) {
  assinatura_quadro_rf433_disponivel = false;
  assinatura_quadro_rf433_capturada = 0;
  uint16_t copia_duracoes[quantidade_maxima_pulsos_rf433];
  int quantidade = 0;

  noInterrupts();
  uint32_t desde_ultima_borda_us = micros() - momento_ultima_borda_rf433_us;
  if (!quadro_rf433_pronto_para_decodificar && receptor_rf433_iniciado &&
      quantidade_pulsos_capturados_rf433 >= quantidade_minima_pulsos_quadro_rf433 &&
      !quadro_rf433_excedeu_pulsos && momento_ultima_borda_rf433_us != 0 &&
      desde_ultima_borda_us >= silencio_fim_quadro_rf433_us) {
    quadro_rf433_pronto_para_decodificar = true;
  }
  if (quadro_rf433_pronto_para_decodificar) {
    quantidade = quantidade_pulsos_capturados_rf433;
    for (int indice = 0; indice < quantidade; indice++) {
      copia_duracoes[indice] = duracoes_pulsos_rf433[indice];
    }
    quantidade_pulsos_capturados_rf433 = 0;
    quadro_rf433_pronto_para_decodificar = false;
    quadro_rf433_excedeu_pulsos = false;
  }
  interrupts();

  if (quantidade <= 0) {
    return false;
  }
  uint32_t assinatura_quadro = 0;
  if (analisaQuadroAssinaturaRf433(copia_duracoes, quantidade, assinatura_quadro, cadastro_controle_rf433_ativo)) {
    assinatura_quadro_rf433_disponivel = true;
    assinatura_quadro_rf433_capturada = assinatura_quadro;
  }
  return decodificaTremDePulsosRf433(copia_duracoes, quantidade, codigo, quantidade_bits);
}

// Escreve no monitor serial o comando binário recebido do controle (o que a IDE mostra no prompt).
void imprimeComandoControleRf433(const String &prefixo, uint32_t codigo, uint8_t quantidade_bits) {
  String bits_texto = "";
  for (int indice = (int)quantidade_bits - 1; indice >= 0; indice--) {
    bits_texto += ((codigo >> indice) & 1u) ? "1" : "0";
  }
  print_log(prefixo + " codigo 0x" + String(codigo, HEX) + " (" + String(quantidade_bits) + " bits) " + bits_texto);
}

// Abre a janela em que o próximo controle repetido é cadastrado. O botão GPIO5 e o teste chamam esta função.
void abreCadastroControleRf433(unsigned long agora) {
  cadastro_controle_rf433_ativo = true;
  momento_inicio_cadastro_controle_rf433 = agora;
  aguardando_repeticao_quadro_rf433 = false;
  print_log("Botao cadastrar controle pressionado (GPIO5). Aperte o botao do controle.");
  print_log("O comando enviado pelo controle aparece neste monitor serial.");
  print_log("Janela de 20 segundos aberta. O quadro RF de cada tecla aparece aqui.");
}

// Diz se este código já está na lista de controles cadastrados.
bool controleRf433JaCadastrado(uint32_t codigo, uint8_t quantidade_bits) {
  for (uint8_t indice = 0; indice < quantidade_controles_rf433_gravados; indice++) {
    if (codigos_controles_rf433[indice] == codigo && bits_controles_rf433[indice] == quantidade_bits) {
      return true;
    }
  }
  return false;
}

// Fecha a captura por silêncio, exige o mesmo quadro duas vezes e, com o botão de cadastrar controle,
// grava a assinatura em buttons (livre, portão 0) e o código binário na lista antiga.
// Botão presente em buttons só pulsa se estiver vinculado a um portão válido.
// Código binário sem entrada em buttons continua abrindo o portão 1.
void processaReceptorFrequencia433(unsigned long agora) {
  if (escuta_botao_cadastrar_controle) {
    static bool leitura_anterior_botao = false;
    static bool botao_pressionado_estavel = false;
    static unsigned long momento_mudanca_botao = 0;
    bool botao_pressionado = digitalRead(pino_botao_cadastrar_controle) == LOW;
    if (botao_pressionado != leitura_anterior_botao) {
      leitura_anterior_botao = botao_pressionado;
      momento_mudanca_botao = agora;
    } else if (botao_pressionado != botao_pressionado_estavel &&
               agora - momento_mudanca_botao >= tempo_estabilizacao_entrada_ms) {
      botao_pressionado_estavel = botao_pressionado;
      if (botao_pressionado_estavel) {
        abreCadastroControleRf433(agora);
      }
    }
  }

  if (cadastro_controle_rf433_ativo &&
      agora - momento_inicio_cadastro_controle_rf433 > tempo_espera_cadastro_controle_rf433_ms) {
    cadastro_controle_rf433_ativo = false;
    print_log("Cadastro de controle encerrado: nenhum comando repetido chegou a tempo.");
  }

  bool tem_quadro = false;
  bool quadro_valido = false;
  bool assinatura_disponivel = false;
  uint32_t codigo = 0;
  uint8_t quantidade_bits = 0;
  uint32_t assinatura_deste_quadro = 0;

  if (quadro_rf433_injetado_pendente) {
    quadro_rf433_injetado_pendente = false;
    tem_quadro = true;
    quadro_valido = quadro_rf433_injetado_valido;
    codigo = codigo_rf433_injetado;
    quantidade_bits = quantidade_bits_codigo_rf433_injetado;
    if (assinatura_rf433_injetada_pendente) {
      assinatura_rf433_injetada_pendente = false;
      assinatura_disponivel = true;
      assinatura_deste_quadro = assinatura_rf433_injetada;
    }
  } else if (consomeQuadroCapturadoRf433(codigo, quantidade_bits)) {
    tem_quadro = true;
    quadro_valido = true;
    assinatura_disponivel = assinatura_quadro_rf433_disponivel;
    assinatura_deste_quadro = assinatura_quadro_rf433_capturada;
    assinatura_quadro_rf433_disponivel = false;
  } else if (assinatura_quadro_rf433_disponivel) {
    tem_quadro = true;
    quadro_valido = false;
    assinatura_disponivel = true;
    assinatura_deste_quadro = assinatura_quadro_rf433_capturada;
    assinatura_quadro_rf433_disponivel = false;
  }

  if (!tem_quadro) {
    return;
  }
  if (!quadro_valido && !assinatura_disponivel) {
    aguardando_repeticao_quadro_rf433 = false;
    assinatura_rf433_candidata_disponivel = false;
    return;
  }

  bool mesma_assinatura = assinatura_disponivel == assinatura_rf433_candidata_disponivel &&
                          (!assinatura_disponivel || assinatura_deste_quadro == assinatura_rf433_candidata);
  if (!aguardando_repeticao_quadro_rf433 || codigo != codigo_rf433_candidato ||
      quantidade_bits != quantidade_bits_codigo_rf433_candidato || !mesma_assinatura) {
    codigo_rf433_candidato = codigo;
    quantidade_bits_codigo_rf433_candidato = quantidade_bits;
    assinatura_rf433_candidata_disponivel = assinatura_disponivel;
    assinatura_rf433_candidata = assinatura_deste_quadro;
    aguardando_repeticao_quadro_rf433 = true;
    if (cadastro_controle_rf433_ativo) {
      if (quadro_valido) {
        imprimeComandoControleRf433("Comando enviado pelo controle:", codigo, quantidade_bits);
      } else {
        print_log("Assinatura do botao de controle recebida. Aperte o mesmo botao outra vez para confirmar.");
      }
    }
    return;
  }
  aguardando_repeticao_quadro_rf433 = false;

  if (cadastro_controle_rf433_ativo) {
    cadastro_controle_rf433_ativo = false;
    if (quadro_valido) {
      imprimeComandoControleRf433("Comando confirmado do controle:", codigo, quantidade_bits);
    }
    if (assinatura_disponivel) {
      adicionaBotaoControleLivre(assinatura_deste_quadro);
    }
    if (!quadro_valido) {
      momento_ultimo_comando_rf433_reconhecido = agora;
      return;
    }
    if (controleRf433JaCadastrado(codigo, quantidade_bits)) {
      if (assinatura_disponivel) {
        print_log("Codigo binario deste botao ja estava na lista antiga. O pulso passa a seguir o status gravado em buttons.");
      } else {
        print_log("Este controle ja estava cadastrado. Aciona-lo abre o portao 1.");
      }
      return;
    }
    if (quantidade_controles_rf433_gravados >= quantidade_maxima_controles_rf433) {
      for (uint8_t indice = 0; indice + 1 < quantidade_maxima_controles_rf433; indice++) {
        codigos_controles_rf433[indice] = codigos_controles_rf433[indice + 1];
        bits_controles_rf433[indice] = bits_controles_rf433[indice + 1];
      }
      quantidade_controles_rf433_gravados = quantidade_maxima_controles_rf433 - 1;
      print_log("Lista de controles cheia. O cadastro mais antigo foi substituido.");
    }
    codigos_controles_rf433[quantidade_controles_rf433_gravados] = codigo;
    bits_controles_rf433[quantidade_controles_rf433_gravados] = quantidade_bits;
    quantidade_controles_rf433_gravados++;
    codigo_rf433_esta_gravado = true;
    codigo_rf433_gravado = codigo;
    quantidade_bits_codigo_rf433_gravado = quantidade_bits;
    gravacao_codigo_rf433_pendente = true;
    momento_ultimo_comando_rf433_reconhecido = agora;
    if (assinatura_disponivel) {
      print_log("Botao de controle cadastrado como livre no portao 0. O pulso so abre o portao depois que o aplicativo vincular.");
    } else {
      print_log("Controle cadastrado. Os proximos acionamentos deste comando abrem o portao 1.");
    }
    return;
  }

  if (assinatura_disponivel) {
    ResultadoConsultaBotaoControle consulta = consultaBotaoControle(assinatura_deste_quadro);
    if (consulta.cadastrado) {
      if (agora - momento_ultimo_comando_rf433_reconhecido < intervalo_minimo_comando_rf433_ms) {
        return;
      }
      momento_ultimo_comando_rf433_reconhecido = agora;
      if (!consulta.pode_pulsar || consulta.numero_portao_pulso == 0) {
        print_log("Botao de controle " + String(consulta.identificador) + " nao acionou portao. Status " +
                  String(consulta.status) + ", portao " + String(consulta.numero_portao) + ".");
        return;
      }
      comando_botao_controle_portao_pendente = consulta.numero_portao_pulso;
      strncpy(identificador_comando_botao_controle_pendente, consulta.identificador,
              tamanho_identificador_botao_controle - 1);
      identificador_comando_botao_controle_pendente[tamanho_identificador_botao_controle - 1] = '\0';
      strncpy(numero_portao_comando_botao_controle_pendente, consulta.numero_portao,
              tamanho_numero_portao_botao_controle - 1);
      numero_portao_comando_botao_controle_pendente[tamanho_numero_portao_botao_controle - 1] = '\0';
      print_log("Botao de controle " + String(consulta.identificador) + " vinculado. Pulso no portao " +
                String(consulta.numero_portao_pulso) + ".");
      return;
    }
  }

  if (!quadro_valido) {
    return;
  }

  if (!controleRf433JaCadastrado(codigo, quantidade_bits)) {
    if (quantidade_controles_rf433_gravados == 0) {
      print_log("Receptor RF433: controle ignorado. Aperte o botao cadastrar controle e aperte o controle de novo.");
    } else {
      print_log("Receptor RF433: codigo diferente dos controles cadastrados, ignorado.");
    }
    return;
  }

  if (agora - momento_ultimo_comando_rf433_reconhecido < intervalo_minimo_comando_rf433_ms) {
    return;
  }

  momento_ultimo_comando_rf433_reconhecido = agora;
  comando_rf433_portao_1_pendente = true;
  imprimeComandoControleRf433("Receptor RF433: comando reconhecido. Abrindo o portao 1.", codigo, quantidade_bits);
}

// Entrega o comando RF433 já reconhecido uma única vez para a tarefa dos portões.
bool consomeComandoReceptorRf433() {
  if (!comando_rf433_portao_1_pendente) {
    return false;
  }
  comando_rf433_portao_1_pendente = false;
  return true;
}

// Captura as durações entre bordas do GPIO48. Pulso fora da faixa zera o quadro; o silêncio fecha a captura.
void IRAM_ATTR trataInterrupcaoReceptorRf433() {
  uint32_t agora_us = micros();
  if (momento_ultima_borda_rf433_us == 0) {
    momento_ultima_borda_rf433_us = agora_us;
    return;
  }

  uint32_t duracao_us = agora_us - momento_ultima_borda_rf433_us;
  momento_ultima_borda_rf433_us = agora_us;
  if (quadro_rf433_pronto_para_decodificar) {
    return;
  }

  if (duracao_us >= silencio_fim_quadro_rf433_us) {
    if (quantidade_pulsos_capturados_rf433 >= quantidade_minima_pulsos_quadro_rf433 && !quadro_rf433_excedeu_pulsos) {
      quadro_rf433_pronto_para_decodificar = true;
    } else {
      quantidade_pulsos_capturados_rf433 = 0;
      quadro_rf433_excedeu_pulsos = false;
    }
    return;
  }

  if (duracao_us < intervalo_minimo_pulso_util_rf433_us || duracao_us > intervalo_maximo_pulso_util_rf433_us) {
    quantidade_pulsos_capturados_rf433 = 0;
    quadro_rf433_excedeu_pulsos = false;
    return;
  }

  if (quantidade_pulsos_capturados_rf433 < quantidade_maxima_pulsos_rf433) {
    duracoes_pulsos_rf433[quantidade_pulsos_capturados_rf433++] = (uint16_t)duracao_us;
  } else {
    quadro_rf433_excedeu_pulsos = true;
  }
}

// Passa a escutar o receptor 433 MHz depois dos testes, para o GPIO48 não interferir no teste_completo.
void iniciaReceptorRf433() {
  quantidade_pulsos_capturados_rf433 = 0;
  quadro_rf433_pronto_para_decodificar = false;
  quadro_rf433_excedeu_pulsos = false;
  momento_ultima_borda_rf433_us = micros();
  receptor_rf433_iniciado = true;
  escuta_botao_cadastrar_controle = true;
  attachInterrupt(digitalPinToInterrupt(pino_receptor_rf433), trataInterrupcaoReceptorRf433, CHANGE);
  print_log("Receptor RF433 escutando no GPIO48. Botao cadastrar controle no GPIO5.");
}

// Carrega os controles RF433 cadastrados. O primeiro continua nas chaves antigas rf433_ok/code/bits.
void carregaCodigoRf433() {
  preferences.begin("portguard", true);
  uint8_t quantidade_lida = preferences.getUChar("rf433_qtd", 0);
  if (quantidade_lida > quantidade_maxima_controles_rf433) {
    quantidade_lida = quantidade_maxima_controles_rf433;
  }
  quantidade_controles_rf433_gravados = 0;
  for (uint8_t indice = 0; indice < quantidade_lida; indice++) {
    String chave_codigo = "rf433_c" + String(indice);
    String chave_bits = "rf433_b" + String(indice);
    uint32_t codigo = preferences.getUInt(chave_codigo.c_str(), 0);
    uint8_t quantidade_bits = preferences.getUChar(chave_bits.c_str(), 0);
    if (quantidade_bits < bits_minimos_quadro_rf433 || quantidade_bits > bits_maximos_quadro_rf433) {
      continue;
    }
    codigos_controles_rf433[quantidade_controles_rf433_gravados] = codigo;
    bits_controles_rf433[quantidade_controles_rf433_gravados] = quantidade_bits;
    quantidade_controles_rf433_gravados++;
  }
  if (quantidade_controles_rf433_gravados == 0 && preferences.getBool("rf433_ok", false)) {
    uint8_t quantidade_bits = preferences.getUChar("rf433_bits", 0);
    if (quantidade_bits >= bits_minimos_quadro_rf433 && quantidade_bits <= bits_maximos_quadro_rf433) {
      codigos_controles_rf433[0] = preferences.getUInt("rf433_code", 0);
      bits_controles_rf433[0] = quantidade_bits;
      quantidade_controles_rf433_gravados = 1;
    }
  }
  preferences.end();

  codigo_rf433_esta_gravado = quantidade_controles_rf433_gravados > 0;
  if (!codigo_rf433_esta_gravado) {
    codigo_rf433_gravado = 0;
    quantidade_bits_codigo_rf433_gravado = 0;
    print_log("Receptor RF433: nenhum controle cadastrado. Aperte o botao cadastrar controle (GPIO5).");
    return;
  }
  codigo_rf433_gravado = codigos_controles_rf433[quantidade_controles_rf433_gravados - 1];
  quantidade_bits_codigo_rf433_gravado = bits_controles_rf433[quantidade_controles_rf433_gravados - 1];
  print_log("Receptor RF433: " + String(quantidade_controles_rf433_gravados) + " controle(s) carregado(s) da Preferences.");
}

// Grava a lista de controles RF433. A tarefa só marca a pendência; quem escreve a Preferences é o loop.
void salvaCodigoRf433() {
  preferences.begin("portguard", false);
  preferences.putUChar("rf433_qtd", quantidade_controles_rf433_gravados);
  for (uint8_t indice = 0; indice < quantidade_controles_rf433_gravados; indice++) {
    String chave_codigo = "rf433_c" + String(indice);
    String chave_bits = "rf433_b" + String(indice);
    preferences.putUInt(chave_codigo.c_str(), codigos_controles_rf433[indice]);
    preferences.putUChar(chave_bits.c_str(), bits_controles_rf433[indice]);
  }
  preferences.putBool("rf433_ok", quantidade_controles_rf433_gravados > 0);
  preferences.putUInt("rf433_code", quantidade_controles_rf433_gravados > 0 ? codigos_controles_rf433[0] : 0);
  preferences.putUChar("rf433_bits", quantidade_controles_rf433_gravados > 0 ? bits_controles_rf433[0] : 0);
  preferences.end();
  gravacao_codigo_rf433_pendente = false;
  print_log("Receptor RF433: " + String(quantidade_controles_rf433_gravados) + " controle(s) persistido(s) na Preferences.");
}

// Carrega da Preferences as flags de intertravamento, fotocélulas dos portões 1, 2 e 3 (padrão: ligadas)
// e modo manutenção (padrão: desligado). A chave "fotocelula" é a do portão 1 (mesma de quando havia uma só).
void carregaConfiguracaoPortoes() {
  preferences.begin("portguard", true);
  intertravamento_ativo = preferences.getBool("intertravamento", true);
  fotocelula_portao_1_alimentada = preferences.getBool("fotocelula", true);
  fotocelula_portao_2_alimentada = preferences.getBool("fotocelula_2", true);
  fotocelula_portao_3_alimentada = preferences.getBool("fotocelula_3", true);
  manutencao_portao_1 = preferences.getBool("manutencao_1", preferences.getBool("manutencao", false));
  manutencao_portao_2 = preferences.getBool("manutencao_2", false);
  manutencao_portao_3 = preferences.getBool("manutencao_3", false);
  preferences.end();

  print_log(String("Intertravamento persistido: ") + (intertravamento_ativo ? "ativo" : "desativado"));
  print_log(String("Fotocelula portao 1 persistida: ") +
            (fotocelula_portao_1_alimentada ? "alimentada (SAIDA 4 em HIGH)" : "desligada (SAIDA 4 em LOW)"));
  print_log(String("Fotocelula portao 2 persistida: ") +
            (fotocelula_portao_2_alimentada ? "alimentada (SAIDA 5 em HIGH)" : "desligada (SAIDA 5 em LOW)"));
  print_log(String("Fotocelula portao 3 persistida: ") +
            (fotocelula_portao_3_alimentada ? "alimentada (SAIDA 6 em HIGH)" : "desligada (SAIDA 6 em LOW)"));
  print_log(String("Manutencao portao 1: ") + (manutencao_portao_1 ? "ativa" : "desativada"));
  print_log(String("Manutencao portao 2: ") + (manutencao_portao_2 ? "ativa" : "desativada"));
  print_log(String("Manutencao portao 3: ") + (manutencao_portao_3 ? "ativa" : "desativada"));
  carregaCodigoRf433();
  carregaBotoesControle();
}

// Salva na Preferences as flags de intertravamento, fotocélulas e modo manutenção para valerem após reiniciar a placa.
void salvaConfiguracaoPortoes() {
  preferences.begin("portguard", false);
  preferences.putBool("intertravamento", intertravamento_ativo);
  preferences.putBool("fotocelula", fotocelula_portao_1_alimentada);
  preferences.putBool("fotocelula_2", fotocelula_portao_2_alimentada);
  preferences.putBool("fotocelula_3", fotocelula_portao_3_alimentada);
  preferences.putBool("manutencao_1", manutencao_portao_1);
  preferences.putBool("manutencao_2", manutencao_portao_2);
  preferences.putBool("manutencao_3", manutencao_portao_3);
  preferences.end();
  print_log("Configuracao dos portoes (intertravamento, fotocelulas e manutencao) persistida na Preferences.");
}

// Tarefa FreeRTOS que, a cada intervalo_tarefa_portoes_ms, lê as 6 entradas, aplica as regras
// dos três portões e atualiza as 8 saídas. Roda separada do loop para que as requisições HTTP
// (que podem levar vários segundos) não atrasem comandos, sensores nem a sirene.
void tarefaControlePortoes(void *parametro) {
  (void)parametro;

  for (;;) {
    unsigned long agora = millis();
    // Tira o comando da fila antes de qualquer I2C. Se a espera do loop estourar, o pulso já foi separado.
    uint8_t comando_remoto_recebido = 0;
    if (fila_comandos_remotos_portao != nullptr) {
      xQueueReceive(fila_comandos_remotos_portao, &comando_remoto_recebido, 0);
    }
    static unsigned long momento_ultima_garantia_entradas = 0;
    if (agora - momento_ultima_garantia_entradas >= 1000) {
      momento_ultima_garantia_entradas = agora;
      garanteConfiguracaoEntradasPca9534();
    }

    bool receptora_bruta_portao_1 = false;
    bool sensor_bruto_portao_1 = false;
    bool receptora_bruta_portao_2 = false;
    bool sensor_bruto_portao_2 = false;
    bool receptora_bruta_portao_3 = false;
    bool sensor_bruto_portao_3 = false;
    leEntradasDeCampo(receptora_bruta_portao_1, sensor_bruto_portao_1, receptora_bruta_portao_2, sensor_bruto_portao_2,
                      receptora_bruta_portao_3, sensor_bruto_portao_3);

    bool receptora_portao_1_acionada = atualizaEntradaEstabilizada(
        entrada_receptora_portao_1, receptora_bruta_portao_1, agora);
    bool sensor_portao_1_fechado = atualizaEntradaEstabilizada(
        entrada_sensor_portao_1, sensor_bruto_portao_1, agora);
    bool receptora_portao_2_acionada = atualizaEntradaEstabilizada(
        entrada_receptora_portao_2, receptora_bruta_portao_2, agora);
    bool sensor_portao_2_fechado = atualizaEntradaEstabilizada(
        entrada_sensor_portao_2, sensor_bruto_portao_2, agora);
    bool receptora_portao_3_acionada = atualizaEntradaEstabilizada(
        entrada_receptora_portao_3, receptora_bruta_portao_3, agora);
    bool sensor_portao_3_fechado = atualizaEntradaEstabilizada(
        entrada_sensor_portao_3, sensor_bruto_portao_3, agora);

    processaReceptorFrequencia433(agora);
    bool comando_rf433 = consomeComandoReceptorRf433();
    char identificador_botao_pulso[tamanho_identificador_botao_controle] = {0};
    char numero_portao_botao_pulso[tamanho_numero_portao_botao_controle] = {0};
    uint8_t portao_do_botao_controle = consomeComandoBotaoControle(identificador_botao_pulso, numero_portao_botao_pulso);

    if (quantidade_portoes_placa < 2) {
      receptora_portao_2_acionada = false;
      sensor_portao_2_fechado = true;
    }
    if (quantidade_portoes_placa < 3) {
      receptora_portao_3_acionada = false;
      sensor_portao_3_fechado = true;
    }

    if (portao_do_botao_controle == 1) {
      receptora_portao_1_acionada = true;
    } else if (portao_do_botao_controle == 2 && quantidade_portoes_placa >= 2) {
      receptora_portao_2_acionada = true;
    } else if (portao_do_botao_controle == 3 && quantidade_portoes_placa >= 3) {
      receptora_portao_3_acionada = true;
    } else if (portao_do_botao_controle != 0) {
      print_log("Pulso do botao de controle ignorado: a placa nao tem o portao " + String(portao_do_botao_controle) + ".");
      portao_do_botao_controle = 0;
    }

    bool bloqueia_portao_1 = sensorBloqueiaIntertravamento(2, sensor_portao_2_fechado) ||
                             sensorBloqueiaIntertravamento(3, sensor_portao_3_fechado);
    bool bloqueia_portao_2 = sensorBloqueiaIntertravamento(1, sensor_portao_1_fechado) ||
                             sensorBloqueiaIntertravamento(3, sensor_portao_3_fechado);
    bool bloqueia_portao_3 = sensorBloqueiaIntertravamento(1, sensor_portao_1_fechado) ||
                             sensorBloqueiaIntertravamento(2, sensor_portao_2_fechado);
    bool comando_portao_1_executado = processaPortao(estado_portao_1, receptora_portao_1_acionada || comando_rf433,
                                                     sensor_portao_1_fechado, agora, bloqueia_portao_1,
                                                     comando_remoto_recebido == 1, portaoEmManutencao(1));
    bool comando_portao_2_executado = true;
    bool comando_portao_3_executado = true;
    if (portaoExisteNaPlaca(2)) {
      comando_portao_2_executado = processaPortao(estado_portao_2, receptora_portao_2_acionada, sensor_portao_2_fechado,
                                                  agora, bloqueia_portao_2, comando_remoto_recebido == 2,
                                                  portaoEmManutencao(2));
    } else if (comando_remoto_recebido == 2) {
      comando_portao_2_executado = false;
    }
    if (portaoExisteNaPlaca(3)) {
      comando_portao_3_executado = processaPortao(estado_portao_3, receptora_portao_3_acionada, sensor_portao_3_fechado,
                                                  agora, bloqueia_portao_3, comando_remoto_recebido == 3,
                                                  portaoEmManutencao(3));
    } else if (comando_remoto_recebido == 3) {
      comando_portao_3_executado = false;
    }

    bool comando_do_botao_executado = false;
    bool sensor_fechado_no_pulso = true;
    if (portao_do_botao_controle == 1) {
      comando_do_botao_executado = comando_portao_1_executado;
      sensor_fechado_no_pulso = sensor_portao_1_fechado;
    } else if (portao_do_botao_controle == 2) {
      comando_do_botao_executado = comando_portao_2_executado;
      sensor_fechado_no_pulso = sensor_portao_2_fechado;
    } else if (portao_do_botao_controle == 3) {
      comando_do_botao_executado = comando_portao_3_executado;
      sensor_fechado_no_pulso = sensor_portao_3_fechado;
    }
    if (portao_do_botao_controle != 0 && comando_do_botao_executado) {
      enfileiraLogPulsoBotaoControle(identificador_botao_pulso, numero_portao_botao_pulso, sensor_fechado_no_pulso);
    } else if (portao_do_botao_controle != 0) {
      print_log("Botao de controle " + String(identificador_botao_pulso) + " nao enviou o comando ao portao " +
                String(portao_do_botao_controle) +
                ": o intertravamento bloqueou a abertura porque outro portao esta aberto.");
    }

    if (comando_remoto_recebido == comando_remoto_desligar_sirene) {
      desligaSirenePortao(estado_portao_1, agora);
      if (portaoExisteNaPlaca(2)) {
        desligaSirenePortao(estado_portao_2, agora);
      }
      if (portaoExisteNaPlaca(3)) {
        desligaSirenePortao(estado_portao_3, agora);
      }
    }

    if (comando_remoto_recebido != 0 && fila_resultados_comandos_remotos != nullptr) {
      bool comando_remoto_executado = true;
      if (comando_remoto_recebido == 1) {
        comando_remoto_executado = comando_portao_1_executado;
      } else if (comando_remoto_recebido == 2) {
        comando_remoto_executado = comando_portao_2_executado;
      } else if (comando_remoto_recebido == 3) {
        comando_remoto_executado = comando_portao_3_executado;
      }
      xQueueSend(fila_resultados_comandos_remotos, &comando_remoto_executado, 0);
    }

    NiveisSaidas niveis_saidas = calculaSaidas(estado_portao_1, estado_portao_2, fotocelula_portao_1_alimentada,
                                              fotocelula_portao_2_alimentada, agora, false, &estado_portao_3,
                                              fotocelula_portao_3_alimentada, quantidade_portoes_placa);
    bool arrombamento_continuo_portao_3 = portaoExisteNaPlaca(3) && !portaoEmManutencao(3) &&
                                          (estado_portao_3.alarme_abertura_indevida_ativo ||
                                           estado_portao_3.sirene_apos_fechamento_indevido_ativa);
    if (arrombamento_continuo_portao_3) {
      niveis_saidas.sirene = HIGH;
      niveis_saidas.central_alarme = LOW;
    }
    aplicaSaidas(niveis_saidas);
    atualizaLedsIndicadoresRede(agora);

    vTaskDelay(pdMS_TO_TICKS(intervalo_tarefa_portoes_ms));
  }
}

// ============================================================
// PREFERENCES
// ============================================================

void carregarConfigWifi() {
  preferences.begin("portguard", true);
  setup_finished = preferences.getBool("setup_finished", false);
  wifi_ssid = preferences.getString("wifi_ssid", "");
  wifi_password = preferences.getString("wifi_password", "");
  ethernetDetectado = preferences.getBool("ethernet_ok", false);
  preferences.end();

  if (setup_finished && wifi_ssid.length() > 0) {
    Serial.println("WiFi carregado da Preferences.");
  } else {
    setup_finished = false;
    wifi_ssid = "";
    wifi_password = "";
    Serial.println("WiFi ainda nao persistido.");
  }

  Serial.print("setup_finished=");
  Serial.println(setup_finished ? "sim" : "nao");
  Serial.print("SSID=");
  Serial.println(wifi_ssid.length() ? wifi_ssid : "(vazio)");
  Serial.print("ethernet_ok persistido=");
  Serial.println(ethernetDetectado ? "sim" : "nao");
}

void salvarConfigWifi() {
  preferences.begin("portguard", false);
  preferences.putBool("setup_finished", setup_finished);
  preferences.putString("wifi_ssid", wifi_ssid);
  preferences.putString("wifi_password", wifi_password);
  preferences.end();
  Serial.println("WiFi persistido na Preferences.");
}

void limparConfigWifi() {
  wifi_ssid = "";
  wifi_password = "";
  setup_finished = false;
  wifiCredenciaisPendentes = false;
  wifiConfigFalhou = false;
  queueIdPendente = 0;
  momentoInicioConfigWifi = 0;
  ultimaTentativaWifi = 0;
  wifiSsidRejeitado = "";
  wifiPasswordRejeitado = "";
  wifi_persistido_ja_iniciado = false;

  salvarConfigWifi();

  // disconnect(true) desliga o rádio com WiFi.mode(WIFI_OFF) e derruba o W5500.
  if (ethIniciado) {
    WiFi.disconnect(false, false);
  } else {
    WiFi.disconnect(true, false);
  }
  delay(100);

  if (!ethIniciado) {
    iniciarEthernet();
  }

  aplicarRotaDeRede();
  Serial.println("WiFi removido. Usando cabo de rede ate novo config_plaque.");
}

void salvarFlagEthernet(bool detectado) {
  ethernetDetectado = detectado;
  preferences.begin("portguard", false);
  preferences.putBool("ethernet_ok", ethernetDetectado);
  preferences.end();

  Serial.print("Flag ethernet_ok persistida=");
  Serial.println(ethernetDetectado ? "sim" : "nao");
}

bool precisaConfigurarWifi() {
  return !wifiEstaConfigurado();
}

// ============================================================
// WI-FI
// ============================================================

String statusWifiTexto() {
  switch (WiFi.status()) {
    case WL_IDLE_STATUS:
      return "IDLE";
    case WL_NO_SSID_AVAIL:
      return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:
      return "SCAN_COMPLETED";
    case WL_CONNECTED:
      return "CONNECTED";
    case WL_CONNECT_FAILED:
      return "CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "DISCONNECTED";
    default:
      return String((int)WiFi.status());
  }
}

void diagnosticarWifi() {
  Serial.println();
  Serial.println("-------- DEBUG WIFI --------");
  Serial.print("SSID: ");
  Serial.println(wifi_ssid);
  Serial.print("Status: ");
  Serial.println(statusWifiTexto());
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("RSSI: ");
  Serial.println(WiFi.RSSI());
  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("Heap livre: ");
  Serial.println(ESP.getFreeHeap());
  Serial.println("----------------------------");
}

// ============================================================
// JSON / API
// ============================================================

String extrairValorJsonString(const String &texto, const char *campo) {
  // JSON normal: "wifi_ssid":"BETO"
  // JSON escapado: \"wifi_ssid\":\"BETO\"
  String padraoNormal = String("\"") + campo + "\":\"";
  String padraoEscapado = String("\\\"") + campo + "\\\":\\\"";

  int inicio = texto.indexOf(padraoNormal);
  bool escapado = false;
  String padrao = padraoNormal;

  if (inicio < 0) {
    inicio = texto.indexOf(padraoEscapado);
    if (inicio < 0) {
      return "";
    }
    escapado = true;
    padrao = padraoEscapado;
  }

  inicio += padrao.length();

  if (escapado) {
    int fim = texto.indexOf("\\\"", inicio);
    if (fim < 0) {
      return "";
    }
    return texto.substring(inicio, fim);
  }

  int fim = texto.indexOf("\"", inicio);
  if (fim < 0) {
    return "";
  }
  return texto.substring(inicio, fim);
}

String extrairValorJsonNumero(const String &texto, const char *campo) {
  String padrao = String("\"") + campo + "\":";
  int inicio = texto.indexOf(padrao);
  if (inicio < 0) {
    return "";
  }

  inicio += padrao.length();
  while (inicio < (int)texto.length() && texto[inicio] == ' ') {
    inicio++;
  }

  int fim = inicio;
  while (fim < (int)texto.length() && isDigit(texto[fim])) {
    fim++;
  }

  if (fim == inicio) {
    return "";
  }

  return texto.substring(inicio, fim);
}

void imprimirCabecalhoRequisicao(const char *destino, bool usarWifi) {
  if (requisicao_sem_log_de_sucesso) {
    return;
  }
  Serial.println();
  Serial.print("Requisição: ");
  Serial.println(destino);
  Serial.print("Meio: ");
  Serial.println(usarWifi ? "WIFI" : "CABO DE REDE");
  Serial.print("IP: ");
  Serial.println(usarWifi ? WiFi.localIP() : ETH.localIP());
  if (usarWifi) {
    Serial.print("ssid: ");
    Serial.println(wifi_ssid.length() ? wifi_ssid : "(vazio)");
    Serial.print("rssi: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  }
}

void imprimirStatusRequisicao(bool ok, const String &erro) {
  if (requisicao_sem_log_de_sucesso) {
    if (!ok) {
      String detalhe_erro = "";
      if (erro.length() > 0) {
        detalhe_erro = " (" + erro + ")";
      }
      print_log("[consulta da fila] erro na requisicao" + detalhe_erro);
    }
    return;
  }
  Serial.print("status requisição: ");
  if (ok) {
    Serial.println("sucesso");
    return;
  }

  Serial.print("erro");
  if (erro.length() > 0) {
    Serial.print(" (");
    Serial.print(erro);
    Serial.print(")");
  }
  Serial.println();
}

String motivoCaboIndisponivel() {
  if (!ethBeginOk) {
    return "W5500 nao iniciou";
  }
  if (ETH.localIP() == IPAddress(0, 0, 0, 0) && !ETH.linkUp()) {
    return "cabo desconectado ou sem link fisico";
  }
  if (ETH.localIP() == IPAddress(0, 0, 0, 0)) {
    return "cabo ligado, mas sem IP (DHCP)";
  }
  return "cabo indisponivel";
}

bool postHttpNoMeio(bool usarWifi, const char *destino, const String &url, const String &body, int timeoutMs, String *payloadOut,
                    const char *tipo_conteudo) {
  imprimirCabecalhoRequisicao(destino, usarWifi);

  if (usarWifi) {
    if (!wifiConectado()) {
      if (!requisicao_sem_log_de_sucesso) {
        imprimirStatusRequisicao(false, String("nao conectou, status=") + statusWifiTexto());
      }
      return false;
    }
    Network.setDefaultInterface(WiFi.STA);
  } else {
    if (!ethIniciado) {
      iniciarEthernet();
    }
    if (!caboConectado()) {
      if (!requisicao_sem_log_de_sucesso) {
        imprimirStatusRequisicao(false, motivoCaboIndisponivel());
      }
      return false;
    }
    Network.setDefaultInterface(ETH);
  }

  httpsClient.stop();
  httpsClient.setInsecure();

  HTTPClient http;
  if (!http.begin(httpsClient, url)) {
    imprimirStatusRequisicao(false, "http.begin() falhou");
    return false;
  }

  http.setTimeout(timeoutMs);
  if (tipo_conteudo == nullptr || tipo_conteudo[0] == '\0') {
    tipo_conteudo = "application/x-www-form-urlencoded";
  }
  http.addHeader("Content-Type", tipo_conteudo);

  int httpCode = http.POST(body);

  if (httpCode <= 0) {
    String erro = http.errorToString(httpCode);
    http.end();
    httpsClient.stop();
    imprimirStatusRequisicao(false, erro);
    return false;
  }

  String payload = http.getString();
  http.end();
  httpsClient.stop();

  if (httpCode != HTTP_CODE_OK && httpCode != HTTP_CODE_CREATED) {
    imprimirStatusRequisicao(false, String("HTTP ") + String(httpCode));
    Serial.print("Resposta: ");
    Serial.println(payload);
    return false;
  }

  if (payloadOut != nullptr) {
    *payloadOut = payload;
  }

  imprimirStatusRequisicao(true, "");
  if (!requisicao_sem_log_de_sucesso) {
    Serial.print("Resposta: ");
    Serial.println(payload);
  }
  return true;
}

bool executarPostHttp(const char *destino, const String &url, const String &body, int timeoutMs, String *payloadOut,
                      const char *tipo_conteudo) {
  if (tipo_conteudo == nullptr || tipo_conteudo[0] == '\0') {
    tipo_conteudo = "application/x-www-form-urlencoded";
  }
  bool tentarWifi = deveTentarWifiAntesDoCabo();

  if (tentarWifi) {
    if (postHttpNoMeio(true, destino, url, body, timeoutMs, payloadOut, tipo_conteudo)) {
      return true;
    }
  }

  preferirCaboSeWifiCaiu();

  if (postHttpNoMeio(false, destino, url, body, timeoutMs, payloadOut, tipo_conteudo)) {
    return true;
  }

  Serial.println("ERRO: WiFi e cabo falharam. Requisicao nao enviada.");
  return false;
}

bool atualizarStatusFila(long queueId, const char *actionStatus) {
  if (queueId <= 0) {
    Serial.println("Sem queue_id para atualizar status da fila.");
    queueIdPendente = 0;
    return false;
  }

  String url = api_url + "/plaque-queues-actions/save";
  String body = "internal_token=" + api_token
                + "&queue_id=" + String(queueId)
                + "&action_status=" + String(actionStatus);

  Serial.println("-------- API FILA SAVE --------");
  Serial.print("queue_id=");
  Serial.print(queueId);
  Serial.print(" action_status=");
  Serial.println(actionStatus);

  bool ok = executarPostHttp("/plaque-queues-actions/save", url, body, 10000, nullptr);
  queueIdPendente = 0;
  queue_id_ultimo_status_respondido = queueId;
  ultimo_status_respondido_fila = actionStatus;
  momento_ultimo_envio_status_fila = millis();
  return ok;
}

// Diz se um gate_broken deve ser descartado por já ter sido enviado outro há menos de
// intervalo_minimo_entre_gate_broken_ms (comparação segura contra o estouro do millis()).
bool gateBrokenDentroDoIntervaloMinimo(unsigned long agora) {
  return gate_broken_ja_enviado && agora - momento_ultimo_gate_broken_enviado < intervalo_minimo_entre_gate_broken_ms;
}

// Monta o texto enviado no event_data quando o sensor de fim de curso falha.
String textoFalhaSensorFimDeCurso(uint8_t numero_portao) {
  return "Falha de leitura no sensor de fim de curso do portão " + String(numero_portao);
}

// JSON do event_data do gate_timeout_open: {"port": N}.
String montaEventDataPortaoAbertoSemFechar(uint8_t numero_portao) {
  return "{\"port\": " + String(numero_portao) + "}";
}

// Codifica um texto para o corpo application/x-www-form-urlencoded.
String codificaTextoParaUrl(const String &texto) {
  const char *digitos_hexadecimais = "0123456789ABCDEF";
  String codificado;
  for (unsigned int indice = 0; indice < texto.length(); indice++) {
    unsigned char caractere = (unsigned char)texto[indice];
    bool caractere_simples = (caractere >= 'a' && caractere <= 'z') || (caractere >= 'A' && caractere <= 'Z') ||
                             (caractere >= '0' && caractere <= '9') || caractere == '-' || caractere == '_' ||
                             caractere == '.' || caractere == '~';
    if (caractere_simples) {
      codificado += (char)caractere;
    } else {
      codificado += '%';
      codificado += digitos_hexadecimais[caractere >> 4];
      codificado += digitos_hexadecimais[caractere & 0x0F];
    }
  }
  return codificado;
}

bool enviarEventoLog(const char *eventType, uint8_t numero_portao) {
  // Evento de um portão em manutenção é descartado. Heartbeat (sem portão) continua sendo enviado.
  if (numero_portao > 0 && portaoEmManutencao(numero_portao)) {
    print_log("Portao " + String(numero_portao) + " em manutencao: evento " + String(eventType) +
              " nao enviado para a API.");
    return true;
  }

  bool evento_gate_broken = strcmp(eventType, "gate_broken") == 0;
  if (evento_gate_broken && gateBrokenDentroDoIntervaloMinimo(millis())) {
    print_log("gate_broken do portao " + String(numero_portao) + " nao enviado: o ultimo gate_broken foi enviado ha " +
              String((millis() - momento_ultimo_gate_broken_enviado) / 1000) + " s (minimo " +
              String(intervalo_minimo_entre_gate_broken_ms / 1000) + " s).");
    return true;
  }

  String url = api_url + "/plaque-events-logs/save";
  String body = "internal_token=" + api_token
                + "&serial=" + plaque_serial
                + "&event_type=" + String(eventType);

  // Falha do sensor vai como texto, com o número do portão, para a notificação do sistema.
  // gate_timeout_open leva o título de portão aberto por mais de 2 minutos e event_data {"port": N}.
  // Os demais eventos de portão seguem event_data = {"port":N}.
  if (strcmp(eventType, "plaque_problem") == 0 && numero_portao > 0) {
    String texto_falha = textoFalhaSensorFimDeCurso(numero_portao);
    body += "&event_title=" + codificaTextoParaUrl("Possivel defeito na placa");
    body += "&event_data=" + codificaTextoParaUrl(texto_falha);
  } else if (strcmp(eventType, tipo_evento_portao_aberto_sem_fechar) == 0 && numero_portao > 0) {
    String event_data = montaEventDataPortaoAbertoSemFechar(numero_portao);
    body += "&event_title=" + codificaTextoParaUrl(titulo_aviso_portao_aberto_sem_fechar);
    body += "&event_data=" + codificaTextoParaUrl(event_data);
    print_log("Requisicao gate_timeout_open do portao " + String(numero_portao) + ": titulo \"" +
              String(titulo_aviso_portao_aberto_sem_fechar) + "\", event_data " + event_data + ".");
  } else if (numero_portao > 0) {
    body += "&event_data=%7B%22port%22%3A" + String(numero_portao) + "%7D";
  }

  Serial.println("-------- DEBUG EVENT LOG --------");
  Serial.print("Evento: ");
  Serial.println(eventType);
  if (numero_portao > 0) {
    print_log("Portao: " + String(numero_portao));
  }

  bool ok = executarPostHttp("/plaque-events-logs/save", url, body, 10000, nullptr);
  if (ok && evento_gate_broken) {
    gate_broken_ja_enviado = true;
    momento_ultimo_gate_broken_enviado = millis();
  }
  Serial.println("--------------------------------");
  return ok;
}

bool chamarApiListagem(String *payloadOut) {
  String url = api_url + "/plaque-queues-actions/list";
  String body = "internal_token=" + api_token
                + "&serial=" + plaque_serial
                + "&limit=1";

  // Consultada a cada meio segundo: só erros aparecem no log da requisição.
  requisicao_sem_log_de_sucesso = true;
  bool sucesso = executarPostHttp("/plaque-queues-actions/list", url, body, 10000, payloadOut);
  requisicao_sem_log_de_sucesso = false;

  consulta_fila_falhou = !sucesso;
  if (!sucesso) {
    momento_ultima_falha_consulta_fila = millis();
  }
  return sucesso;
}

bool processarResetPlaque(long queueId) {
  Serial.println();
  Serial.println("Evento reset_plaque recebido.");
  Serial.println("Limpando configuracao WiFi da placa.");

  if (queueId > 0 && queueId != ultimoQueueIdNotificado) {
    if (atualizarStatusFila(queueId, "completed")) {
      ultimoQueueIdNotificado = queueId;
    }
  }

  limparConfigWifi();
  return true;
}

// Lê o campo action_data da resposta da fila e devolve o valor bruto, sem espaços nas pontas.
// Ausente ou null viram texto vazio. Aceita número (2), texto ("2") ou JSON, normal ou escapado ({"port":2}).
String extraiValorActionData(const String &payload) {
  const String campo_action_data = "\"action_data\":";
  int inicio = payload.indexOf(campo_action_data);
  if (inicio < 0) {
    return "";
  }

  int tamanho_payload = payload.length();
  inicio += campo_action_data.length();
  while (inicio < tamanho_payload && payload[inicio] == ' ') {
    inicio++;
  }

  String valor_action_data;
  if (inicio < tamanho_payload && payload[inicio] == '"') {
    int fim = inicio + 1;
    while (fim < tamanho_payload && !(payload[fim] == '"' && payload[fim - 1] != '\\')) {
      fim++;
    }
    valor_action_data = payload.substring(inicio + 1, fim);
  } else if (inicio < tamanho_payload && payload[inicio] == '{') {
    int fim = inicio;
    int profundidade_chaves = 0;
    while (fim < tamanho_payload) {
      if (payload[fim] == '{') {
        profundidade_chaves++;
      } else if (payload[fim] == '}') {
        profundidade_chaves--;
        if (profundidade_chaves == 0) {
          fim++;
          break;
        }
      }
      fim++;
    }
    valor_action_data = payload.substring(inicio, fim);
  } else {
    int fim = inicio;
    while (fim < tamanho_payload && payload[fim] != ',' && payload[fim] != '}') {
      fim++;
    }
    valor_action_data = payload.substring(inicio, fim);
  }

  valor_action_data.trim();
  if (valor_action_data == "null") {
    return "";
  }
  return valor_action_data;
}

// Devolve a primeira sequência de dígitos encontrada no texto ("{\"port\":2}" -> "2"), ou vazio se não houver.
String extraiPrimeiroNumeroDoTexto(const String &texto) {
  int inicio_numero = 0;
  while (inicio_numero < (int)texto.length() && !isDigit(texto[inicio_numero])) {
    inicio_numero++;
  }
  int fim_numero = inicio_numero;
  while (fim_numero < (int)texto.length() && isDigit(texto[fim_numero])) {
    fim_numero++;
  }
  return texto.substring(inicio_numero, fim_numero);
}

// Devolve o portão do open_or_close_gate a partir do action_data:
// ausente, vazio, null ou 1 = portão 1; 2 = portão 2; 3 = portão 3; qualquer outro valor = 0 (inválido).
uint8_t extraiNumeroPortaoDoActionData(const String &payload) {
  String valor_action_data = extraiValorActionData(payload);
  if (valor_action_data.length() == 0) {
    return 1;
  }

  String numero_portao = extraiPrimeiroNumeroDoTexto(valor_action_data);
  if (numero_portao == "1") {
    return 1;
  }
  if (numero_portao == "2") {
    return 2;
  }
  if (numero_portao == "3") {
    return 3;
  }
  return 0;
}

// Devolve a flag das ações intertravamento e fotocelula a partir do action_data:
// 0 ou false = 0; 1 ou true = 1; ausente, vazio ou qualquer outro valor = -1 (inválido).
// Se houver a chave "value" (ex.: {"port":2,"value":0}), lê somente o valor dela.
int extraiFlagDoActionData(const String &payload) {
  String valor_action_data = extraiValorActionData(payload);
  valor_action_data.toLowerCase();

  int indice_chave_value = valor_action_data.indexOf("value");
  if (indice_chave_value < 0) {
    indice_chave_value = valor_action_data.indexOf("enabled");
  }
  if (indice_chave_value >= 0) {
    valor_action_data = valor_action_data.substring(indice_chave_value + strlen("value"));
    while (valor_action_data.length() > 0 && !isAlphaNumeric(valor_action_data[0])) {
      valor_action_data.remove(0, 1);
    }
    if (valor_action_data.startsWith("true")) {
      return 1;
    }
    if (valor_action_data.startsWith("false")) {
      return 0;
    }
  } else if (valor_action_data.indexOf("true") >= 0) {
    return 1;
  } else if (valor_action_data.indexOf("false") >= 0) {
    return 0;
  }

  String numero_flag = extraiPrimeiroNumeroDoTexto(valor_action_data);
  if (numero_flag == "0") {
    return 0;
  }
  if (numero_flag == "1") {
    return 1;
  }
  return -1;
}

// Devolve o portão de uma ação de fotocélula. O número no tipo vale para fotocelula_1/2/3 e também
// para fotocelula_port_3. Sem número no tipo, lê a chave "port" do action_data ou do payload
// ({"port":3,"value":1}): sem "port" = portão 1; 1, 2 ou 3 = esse portão; outro valor = 0 (inválido).
uint8_t extraiPortaoFotocelula(const String &tipo_acao, const String &payload) {
  if (tipo_acao.startsWith("fotocelula")) {
    String numero_no_tipo = extraiPrimeiroNumeroDoTexto(tipo_acao);
    if (numero_no_tipo == "1") {
      return 1;
    }
    if (numero_no_tipo == "2") {
      return 2;
    }
    if (numero_no_tipo == "3") {
      return 3;
    }
  }

  String valor_action_data = extraiValorActionData(payload);
  int indice_chave_port = valor_action_data.indexOf("port");
  String texto_do_portao = valor_action_data;
  if (indice_chave_port < 0) {
    indice_chave_port = payload.indexOf("\"port\"");
    texto_do_portao = payload;
  }
  if (indice_chave_port < 0) {
    return 1;
  }

  String numero_portao = extraiPrimeiroNumeroDoTexto(texto_do_portao.substring(indice_chave_port + strlen("port")));
  if (numero_portao == "1") {
    return 1;
  }
  if (numero_portao == "2") {
    return 2;
  }
  if (numero_portao == "3") {
    return 3;
  }
  return 0;
}

// Devolve o portão de maintenance_port_1, _2 ou _3. set/unset e qualquer outro tipo devolvem 0.
uint8_t numeroPortaoDaAcaoManutencao(const String &tipo_acao) {
  if (tipo_acao == "maintenance_port_1") {
    return 1;
  }
  if (tipo_acao == "maintenance_port_2") {
    return 2;
  }
  if (tipo_acao == "maintenance_port_3") {
    return 3;
  }
  return 0;
}

// Diz se a ação da fila é manutenção de portão (por portão ou o par antigo set/unset, aplicado a todos).
bool acaoEhManutencao(const String &tipo_acao) {
  return tipo_acao == "set_maintenance" || tipo_acao == "unset_maintenance" || tipo_acao.startsWith("maintenance_port_");
}

// Diz se uma ação imediata da fila (open_or_close_gate, down_siren) é antiga demais para ser executada:
// created_at (horário de Brasília) mais de validade_acao_imediata_fila_segundos antes de agora_epoch (UTC, NTP).
// Sem relógio sincronizado, sem created_at válido ou com created_at no futuro, não considera expirada.
bool acaoDaFilaExpirada(const String &payload, long long agora_epoch) {
  if (agora_epoch < epoch_minimo_relogio_sincronizado) {
    return false;
  }

  String criado_em = extrairValorJsonString(payload, "created_at");
  int ano, mes, dia, hora, minuto, segundo;
  if (sscanf(criado_em.c_str(), "%d-%d-%d %d:%d:%d", &ano, &mes, &dia, &hora, &minuto, &segundo) != 6 ||
      mes < 1 || mes > 12 || dia < 1 || dia > 31) {
    return false;
  }

  // Dias desde 01/01/1970 no calendário gregoriano (algoritmo days_from_civil).
  long long ano_ajustado = mes <= 2 ? ano - 1 : ano;
  long long era = (ano_ajustado >= 0 ? ano_ajustado : ano_ajustado - 399) / 400;
  long long ano_da_era = ano_ajustado - era * 400;
  long long dia_do_ano = (153 * (mes > 2 ? mes - 3 : mes + 9) + 2) / 5 + dia - 1;
  long long dia_da_era = ano_da_era * 365 + ano_da_era / 4 - ano_da_era / 100 + dia_do_ano;
  long long dias_desde_1970 = era * 146097 + dia_da_era - 719468;

  long long criado_em_epoch = dias_desde_1970 * 86400 + hora * 3600LL + minuto * 60LL + segundo - fuso_horario_servidor_segundos;
  return agora_epoch - criado_em_epoch > validade_acao_imediata_fila_segundos;
}

// Trata as ações de flag da fila e persiste na Preferences para valer após reiniciar, respondendo completed ou failed:
// - intertravamento: aplica a flag do action_data (0 = false, 1 = true);
// - fotocelula_1 / fotocelula_2 / fotocelula_3: action_data 0 coloca a fotocélula do portão 1 (SAÍDA 4) /
//   portão 2 (SAÍDA 5) / portão 3 (SAÍDA 6) em LOW e 1 em HIGH.
//   fotocelula (sem número) aceita {"port":N,"value":V} ou só 0/1 (portão 1).
// - maintenance_port_1 / _2 / _3: action_data 0 tira e 1 coloca aquele portão em manutenção.
//   set_maintenance / unset_maintenance aplicam o mesmo a todos os portões da placa.
bool processarAlteracaoFlagPortoes(long queue_id, const String &tipo_acao, const String &payload) {
  bool acao_manutencao = acaoEhManutencao(tipo_acao);
  bool manutencao_em_todos = tipo_acao == "set_maintenance" || tipo_acao == "unset_maintenance";
  bool acao_fotocelula = tipo_acao.startsWith("fotocelula");
  int valor_flag = tipo_acao == "set_maintenance" ? 1 : (tipo_acao == "unset_maintenance" ? 0 : extraiFlagDoActionData(payload));
  uint8_t numero_portao_fotocelula = acao_fotocelula ? extraiPortaoFotocelula(tipo_acao, payload) : 0;
  uint8_t numero_portao_manutencao = numeroPortaoDaAcaoManutencao(tipo_acao);
  print_log("");
  print_log("Evento " + tipo_acao + " recebido. Valor solicitado: " +
            (valor_flag >= 0 ? String(valor_flag) : String("invalido")));
  if (acao_fotocelula) {
    print_log("Portao da fotocelula: " +
              (numero_portao_fotocelula > 0 ? String(numero_portao_fotocelula) : String("invalido")));
  }

  if (queue_id <= 0) {
    print_log(tipo_acao + " sem queue_id: alteracao nao aplicada.");
    return false;
  }
  ultimoQueueIdNotificado = queue_id;

  bool fotocelula_fora_da_placa = acao_fotocelula &&
                                  (numero_portao_fotocelula == 0 || !portaoExisteNaPlaca(numero_portao_fotocelula));
  bool manutencao_fora_da_placa = acao_manutencao && !manutencao_em_todos &&
                                  (numero_portao_manutencao == 0 || !portaoExisteNaPlaca(numero_portao_manutencao));
  if (valor_flag < 0 || fotocelula_fora_da_placa || manutencao_fora_da_placa) {
    print_log(tipo_acao + " com action_data invalido ou portao fora desta placa (use 0 ou 1; portao 1 a " +
              String(quantidade_portoes_placa) + ").");
    atualizarStatusFila(queue_id, "failed");
    return false;
  }

  if (manutencao_em_todos) {
    for (uint8_t numero_portao = 1; numero_portao <= quantidade_portoes_placa; numero_portao++) {
      defineManutencaoPortao(numero_portao, valor_flag == 1);
    }
  } else if (acao_manutencao) {
    defineManutencaoPortao(numero_portao_manutencao, valor_flag == 1);
  } else if (tipo_acao == "intertravamento") {
    defineIntertravamento(valor_flag == 1);
  } else {
    defineAlimentacaoFotocelula(numero_portao_fotocelula, valor_flag == 1);
  }
  salvaConfiguracaoPortoes();

  atualizarStatusFila(queue_id, "completed");
  return true;
}

// Entrega um comando da fila da API (1/2/3 = open_or_close_gate do portão, comando_remoto_desligar_sirene = down_siren)
// para a tarefa dos portões e espera o resultado. Retorna false se a tarefa não estiver rodando ou não responder;
// comando_executado recebe o resultado (false quando o intertravamento bloqueou a abertura).
bool enviaComandoRemotoParaTarefaPortoes(uint8_t comando_remoto, bool &comando_executado) {
  comando_executado = false;
  if (fila_comandos_remotos_portao == nullptr || fila_resultados_comandos_remotos == nullptr ||
      tarefa_controle_portoes == nullptr) {
    print_log("ERRO: controle dos portoes indisponivel.");
    return false;
  }

  xQueueReset(fila_resultados_comandos_remotos);
  bool comando_entregue = xQueueSend(fila_comandos_remotos_portao, &comando_remoto, pdMS_TO_TICKS(100)) == pdTRUE;
  bool resultado_recebido = comando_entregue &&
                            xQueueReceive(fila_resultados_comandos_remotos, &comando_executado,
                                          pdMS_TO_TICKS(tempo_espera_resultado_comando_remoto_ms)) == pdTRUE;
  if (!resultado_recebido) {
    xQueueReset(fila_comandos_remotos_portao);
    comando_executado = false;
  }
  return resultado_recebido;
}

// Converte o button_id (decimal ou assinatura hexadecimal) para o número da assinatura capturada.
bool converteIdentificadorBotaoControle(const String &texto, uint32_t &assinatura) {
  String limpo = texto;
  limpo.trim();
  if (limpo.startsWith("\"")) {
    limpo = limpo.substring(1);
    limpo.trim();
  }
  if (limpo.endsWith("\"")) {
    limpo.remove(limpo.length() - 1);
    limpo.trim();
  }
  if (limpo.length() == 0) {
    return false;
  }

  int base = 10;
  if (limpo.length() > 2 && (limpo.startsWith("0x") || limpo.startsWith("0X"))) {
    base = 16;
    limpo = limpo.substring(2);
  } else {
    for (unsigned int indice = 0; indice < limpo.length(); indice++) {
      char caractere = limpo[indice];
      bool digito = caractere >= '0' && caractere <= '9';
      bool hexadecimal = (caractere >= 'a' && caractere <= 'f') || (caractere >= 'A' && caractere <= 'F');
      if (hexadecimal) {
        base = 16;
      } else if (!digito) {
        return false;
      }
    }
  }
  if (limpo.length() == 0) {
    return false;
  }

  char *fim = nullptr;
  unsigned long valor = strtoul(limpo.c_str(), &fim, base);
  if (fim == nullptr || fim == limpo.c_str() || *fim != '\0') {
    return false;
  }
  assinatura = (uint32_t)valor;
  return true;
}

// Diz se o identificador gravado é a mesma assinatura, sem alocar texto.
bool assinaturaConfereComIdentificadorArmazenado(const char *identificador, uint32_t assinatura) {
  if (identificador == nullptr || identificador[0] == '\0') {
    return false;
  }
  char *fim = nullptr;
  unsigned long valor = strtoul(identificador, &fim, 10);
  if (fim == identificador || (fim != nullptr && *fim != '\0')) {
    return false;
  }
  return (uint32_t)valor == assinatura;
}

// Texto decimal da assinatura. É o button_id gravado e enviado à API.
String textoIdentificadorBotaoControle(uint32_t assinatura) {
  char texto[tamanho_identificador_botao_controle];
  snprintf(texto, sizeof(texto), "%lu", (unsigned long)assinatura);
  return String(texto);
}

// Escreve o array JSON de uma cópia da lista num buffer fixo, sem depender do heap.
// Se não couber a lista inteira, não escreve um JSON pela metade.
bool escreveJsonDaCopiaBotoesControle(const BotaoControleCadastrado *copia, uint8_t quantidade, char *buffer,
                                     size_t tamanho_buffer) {
  if (buffer == nullptr || tamanho_buffer < 3) {
    return false;
  }

  size_t usado = 0;
  buffer[usado++] = '[';
  buffer[usado] = '\0';
  bool primeiro = true;
  for (uint8_t indice = 0; indice < quantidade; indice++) {
    if (!copia[indice].ocupado) {
      continue;
    }

    const char *pendencia = "";
    if (copia[indice].operacao_api_pendente == operacao_api_botao_cadastrar) {
      pendencia = ", \"api_pendente\": \"cadastrar\"";
    } else if (copia[indice].operacao_api_pendente == operacao_api_botao_resetar) {
      pendencia = ", \"api_pendente\": \"resetar\"";
    }

    char objeto[160];
    int escrito = snprintf(objeto, sizeof(objeto),
                           "{\"button_id\": \"%s\", \"port_number\": \"%s\", \"status\": \"%s\"%s}",
                           copia[indice].identificador, copia[indice].numero_portao, copia[indice].status, pendencia);
    if (escrito < 0 || (size_t)escrito >= sizeof(objeto)) {
      buffer[0] = '\0';
      return false;
    }
    size_t precisa = (size_t)escrito + (primeiro ? 0 : 1);
    if (usado + precisa + 2 > tamanho_buffer) {
      buffer[0] = '\0';
      return false;
    }
    if (!primeiro) {
      buffer[usado++] = ',';
    }
    memcpy(buffer + usado, objeto, (size_t)escrito);
    usado += (size_t)escrito;
    buffer[usado] = '\0';
    primeiro = false;
  }

  if (usado + 2 > tamanho_buffer) {
    buffer[0] = '\0';
    return false;
  }
  buffer[usado++] = ']';
  buffer[usado] = '\0';
  return true;
}

// Monta o array JSON de botões a partir de uma cópia já tirada da lista.
String montaJsonDaCopiaBotoesControle(const BotaoControleCadastrado *copia, uint8_t quantidade) {
  if (!escreveJsonDaCopiaBotoesControle(copia, quantidade, buffer_json_botoes_controle,
                                        tamanho_buffer_json_botoes_controle)) {
    return "[]";
  }
  return String(buffer_json_botoes_controle);
}

// Monta o array JSON atualmente gravado em memória, no formato do índice buttons.
String montaJsonBotoesControle() {
  BotaoControleCadastrado copia[quantidade_maxima_botoes_controle];
  uint8_t quantidade = 0;
  portENTER_CRITICAL(&protecao_botoes_controle);
  quantidade = quantidade_botoes_controle;
  memcpy(copia, botoes_controle, sizeof(copia));
  portEXIT_CRITICAL(&protecao_botoes_controle);
  return montaJsonDaCopiaBotoesControle(copia, quantidade);
}

// Lê um campo de um objeto de botão, com ou sem aspas e com espaço depois dos dois-pontos.
String extraiCampoObjetoBotaoControle(const String &objeto, const char *campo) {
  String chave = String("\"") + campo + "\"";
  int indice = objeto.indexOf(chave);
  if (indice < 0) {
    return "";
  }
  indice += chave.length();
  while (indice < (int)objeto.length() &&
         (objeto[indice] == ' ' || objeto[indice] == '\t' || objeto[indice] == ':')) {
    indice++;
  }
  if (indice >= (int)objeto.length()) {
    return "";
  }
  if (objeto[indice] == '"') {
    int fim = objeto.indexOf('"', indice + 1);
    if (fim < 0) {
      return "";
    }
    return objeto.substring(indice + 1, fim);
  }
  int fim = indice;
  while (fim < (int)objeto.length() && objeto[fim] != ',' && objeto[fim] != '}') {
    fim++;
  }
  String valor = objeto.substring(indice, fim);
  valor.trim();
  return valor;
}

// Tira o escape de um action_data que veio como texto dentro do JSON da fila.
// Repete poucas vezes para o caso de o servidor ter escapado o JSON mais de uma vez.
String normalizaJsonBotaoControle(const String &texto) {
  String normalizado = texto;
  for (uint8_t tentativa = 0; tentativa < 3; tentativa++) {
    if (normalizado.indexOf("\\\"") < 0) {
      break;
    }
    normalizado.replace("\\\"", "\"");
  }
  return normalizado;
}

// Substitui a lista em memória pelo array JSON, ou só acrescenta os botões que a memória perdeu.
// substituir_lista true recupera cadastro ou reset que ainda não foi confirmado pela API.
// substituir_lista false não apaga nem altera o que já está na memória.
uint8_t interpretaJsonBotoesControle(const String &json, bool substituir_lista = true) {
  BotaoControleCadastrado temporarios[quantidade_maxima_botoes_controle];
  memset(temporarios, 0, sizeof(temporarios));
  uint8_t quantidade_lidos = 0;
  int cursor = 0;
  while (cursor < (int)json.length() && quantidade_lidos < quantidade_maxima_botoes_controle) {
    int inicio = json.indexOf('{', cursor);
    if (inicio < 0) {
      break;
    }
    int fim = json.indexOf('}', inicio);
    if (fim < 0) {
      break;
    }
    String objeto = json.substring(inicio, fim + 1);
    String identificador = extraiCampoObjetoBotaoControle(objeto, "button_id");
    String numero_portao = extraiCampoObjetoBotaoControle(objeto, "port_number");
    String status = extraiCampoObjetoBotaoControle(objeto, "status");
    String envio_pendente = extraiCampoObjetoBotaoControle(objeto, "api_pendente");
    identificador.trim();
    numero_portao.trim();
    status.trim();
    status.toLowerCase();
    uint32_t assinatura = 0;
    bool identificador_valido = converteIdentificadorBotaoControle(identificador, assinatura);
    bool status_valido = status == "livre" || status == "vinculado" || status == "desativado";
    bool porta_valida = numero_portao == "0" || numero_portao == "1" || numero_portao == "2" || numero_portao == "3";
    if (identificador_valido && status_valido && porta_valida) {
      snprintf(temporarios[quantidade_lidos].identificador, tamanho_identificador_botao_controle, "%lu",
               (unsigned long)assinatura);
      snprintf(temporarios[quantidade_lidos].numero_portao, tamanho_numero_portao_botao_controle, "%s",
               numero_portao.c_str());
      snprintf(temporarios[quantidade_lidos].status, tamanho_status_botao_controle, "%s", status.c_str());
      envio_pendente.trim();
      envio_pendente.toLowerCase();
      if (envio_pendente == "cadastrar") {
        temporarios[quantidade_lidos].operacao_api_pendente = operacao_api_botao_cadastrar;
      } else if (envio_pendente == "resetar") {
        temporarios[quantidade_lidos].operacao_api_pendente = operacao_api_botao_resetar;
      }
      temporarios[quantidade_lidos].ocupado = true;
      quantidade_lidos++;
    }
    cursor = fim + 1;
  }

  if (substituir_lista) {
    portENTER_CRITICAL(&protecao_botoes_controle);
    memset(botoes_controle, 0, sizeof(botoes_controle));
    memcpy(botoes_controle, temporarios, sizeof(temporarios));
    quantidade_botoes_controle = quantidade_lidos;
    portEXIT_CRITICAL(&protecao_botoes_controle);
    return quantidade_lidos;
  }

  uint32_t assinaturas_lidas[quantidade_maxima_botoes_controle];
  bool assinatura_valida[quantidade_maxima_botoes_controle];
  memset(assinaturas_lidas, 0, sizeof(assinaturas_lidas));
  memset(assinatura_valida, 0, sizeof(assinatura_valida));
  for (uint8_t indice = 0; indice < quantidade_lidos; indice++) {
    assinatura_valida[indice] =
        converteIdentificadorBotaoControle(temporarios[indice].identificador, assinaturas_lidas[indice]);
  }

  uint8_t acrescentados = 0;
  portENTER_CRITICAL(&protecao_botoes_controle);
  for (uint8_t indice = 0; indice < quantidade_lidos; indice++) {
    if (!assinatura_valida[indice] || quantidade_botoes_controle >= quantidade_maxima_botoes_controle) {
      continue;
    }
    bool ja_existe = false;
    for (uint8_t atual = 0; atual < quantidade_botoes_controle; atual++) {
      if (botoes_controle[atual].ocupado &&
          assinaturaConfereComIdentificadorArmazenado(botoes_controle[atual].identificador, assinaturas_lidas[indice])) {
        ja_existe = true;
        break;
      }
    }
    if (ja_existe) {
      continue;
    }
    botoes_controle[quantidade_botoes_controle] = temporarios[indice];
    quantidade_botoes_controle++;
    acrescentados++;
  }
  if (acrescentados > 0) {
    geracao_botoes_controle++;
    gravacao_botoes_controle_pendente = true;
  }
  portEXIT_CRITICAL(&protecao_botoes_controle);
  return acrescentados;
}

// Carrega o índice buttons da Preferences para a lista em memória.
void carregaBotoesControle() {
  preferences.begin("portguard", true);
  String json = preferences.getString(chave_armazenamento_botoes_controle, "[]");
  preferences.end();
  uint8_t quantidade = interpretaJsonBotoesControle(json);
  print_log("Botoes de controle carregados de buttons: " + String(quantidade) + ".");
}

// Grava a lista inteira no índice buttons e só encerra a pendência se a leitura devolver o mesmo JSON.
// Uma gravação cortada ou antiga não apaga o que a memória já atualizou.
bool salvaBotoesControle() {
  if (momento_proxima_gravacao_botoes_controle != 0 &&
      (long)(millis() - momento_proxima_gravacao_botoes_controle) < 0) {
    return false;
  }

  for (uint8_t tentativa = 0; tentativa < 3; tentativa++) {
    BotaoControleCadastrado copia[quantidade_maxima_botoes_controle];
    uint8_t quantidade = 0;
    uint32_t geracao_vista = 0;
    portENTER_CRITICAL(&protecao_botoes_controle);
    if (!gravacao_botoes_controle_pendente) {
      portEXIT_CRITICAL(&protecao_botoes_controle);
      momento_proxima_gravacao_botoes_controle = 0;
      return true;
    }
    quantidade = quantidade_botoes_controle;
    geracao_vista = geracao_botoes_controle;
    memcpy(copia, botoes_controle, sizeof(copia));
    portEXIT_CRITICAL(&protecao_botoes_controle);

    if (!escreveJsonDaCopiaBotoesControle(copia, quantidade, buffer_json_botoes_controle,
                                          tamanho_buffer_json_botoes_controle)) {
      print_log("ERRO: a lista de botoes nao cabe na gravacao. A Preferences nao foi alterada.");
      momento_proxima_gravacao_botoes_controle = millis() + intervalo_nova_tentativa_gravacao_botoes_controle_ms;
      return false;
    }

    if (!preferences.begin("portguard", false)) {
      print_log("ERRO: nao foi possivel abrir a Preferences para gravar os botoes.");
      momento_proxima_gravacao_botoes_controle = millis() + intervalo_nova_tentativa_gravacao_botoes_controle_ms;
      return false;
    }

    size_t tamanho_esperado = strlen(buffer_json_botoes_controle);
    size_t tamanho_gravado = preferences.putString(chave_armazenamento_botoes_controle, buffer_json_botoes_controle);
    if (tamanho_gravado != tamanho_esperado) {
      preferences.remove(chave_armazenamento_botoes_controle);
      tamanho_gravado = preferences.putString(chave_armazenamento_botoes_controle, buffer_json_botoes_controle);
    }
    String json_lido = preferences.getString(chave_armazenamento_botoes_controle, "");
    preferences.end();

    bool conferiu = tamanho_gravado == tamanho_esperado && json_lido == buffer_json_botoes_controle;
    bool geracao_mudou = false;
    portENTER_CRITICAL(&protecao_botoes_controle);
    geracao_mudou = geracao_botoes_controle != geracao_vista;
    if (conferiu && !geracao_mudou) {
      gravacao_botoes_controle_pendente = false;
    }
    portEXIT_CRITICAL(&protecao_botoes_controle);

    if (conferiu && !geracao_mudou) {
      momento_proxima_gravacao_botoes_controle = 0;
      print_log("Botoes de controle persistidos em buttons: " + String(quantidade) +
                " botao(oes), leitura conferida.");
      return true;
    }
    if (!conferiu) {
      print_log("ERRO: a Preferences nao devolveu a mesma lista de botoes. Nova tentativa de gravacao.");
    }
  }

  momento_proxima_gravacao_botoes_controle = millis() + intervalo_nova_tentativa_gravacao_botoes_controle_ms;
  print_log("ERRO: os botoes continuam pendentes de gravacao na Preferences.");
  return false;
}

// Inclui a assinatura como botão livre no portão 0. Se ela já existe, reseta para livre/portão 0.
bool adicionaBotaoControleLivre(uint32_t assinatura) {
  char identificador[tamanho_identificador_botao_controle];
  snprintf(identificador, sizeof(identificador), "%lu", (unsigned long)assinatura);
  bool ja_existia = false;
  bool sem_espaco = false;

  portENTER_CRITICAL(&protecao_botoes_controle);
  for (uint8_t indice = 0; indice < quantidade_botoes_controle; indice++) {
    if (botoes_controle[indice].ocupado &&
        assinaturaConfereComIdentificadorArmazenado(botoes_controle[indice].identificador, assinatura)) {
      snprintf(botoes_controle[indice].numero_portao, tamanho_numero_portao_botao_controle, "0");
      snprintf(botoes_controle[indice].status, tamanho_status_botao_controle, "livre");
      botoes_controle[indice].operacao_api_pendente = operacao_api_botao_resetar;
      geracao_botoes_controle++;
      gravacao_botoes_controle_pendente = true;
      ja_existia = true;
      break;
    }
  }
  if (!ja_existia) {
    if (quantidade_botoes_controle >= quantidade_maxima_botoes_controle) {
      sem_espaco = true;
    } else {
      BotaoControleCadastrado *botao = &botoes_controle[quantidade_botoes_controle];
      memset(botao, 0, sizeof(*botao));
      snprintf(botao->identificador, tamanho_identificador_botao_controle, "%s", identificador);
      snprintf(botao->numero_portao, tamanho_numero_portao_botao_controle, "0");
      snprintf(botao->status, tamanho_status_botao_controle, "livre");
      botao->operacao_api_pendente = operacao_api_botao_cadastrar;
      botao->ocupado = true;
      quantidade_botoes_controle++;
      geracao_botoes_controle++;
      gravacao_botoes_controle_pendente = true;
    }
  }
  portEXIT_CRITICAL(&protecao_botoes_controle);

  char hexadecimal[12];
  snprintf(hexadecimal, sizeof(hexadecimal), "%08lX", (unsigned long)assinatura);
  if (ja_existia) {
    print_log("Botao de controle ja cadastrado (button_id " + String(identificador) + ", assinatura 0x" +
              String(hexadecimal) + "). Resetando para livre no portao 0 e enviando a atualizacao para a API.");
    return true;
  }
  if (sem_espaco) {
    print_log("Lista de botoes de controle cheia. Cadastro nao gravado.");
    return false;
  }

  print_log("Botao de controle cadastrado: button_id " + String(identificador) + " (assinatura 0x" +
            String(hexadecimal) + "), portao 0, status livre.");
  return true;
}

// Atualiza status e portão do botão cuja assinatura confere com o button_id recebido.
bool atualizaBotaoControleArmazenado(const String &identificador_recebido, const String &numero_portao, const String &status) {
  uint32_t assinatura = 0;
  if (!converteIdentificadorBotaoControle(identificador_recebido, assinatura)) {
    return false;
  }
  String status_normalizado = status;
  status_normalizado.trim();
  status_normalizado.toLowerCase();
  String numero_normalizado = numero_portao;
  numero_normalizado.trim();
  if (numero_normalizado != "0" && numero_normalizado != "1" && numero_normalizado != "2" && numero_normalizado != "3") {
    return false;
  }
  if (status_normalizado != "livre" && status_normalizado != "vinculado" && status_normalizado != "desativado") {
    return false;
  }

  bool encontrou = false;
  portENTER_CRITICAL(&protecao_botoes_controle);
  for (uint8_t indice = 0; indice < quantidade_botoes_controle; indice++) {
    if (!botoes_controle[indice].ocupado ||
        !assinaturaConfereComIdentificadorArmazenado(botoes_controle[indice].identificador, assinatura)) {
      continue;
    }
    snprintf(botoes_controle[indice].numero_portao, tamanho_numero_portao_botao_controle, "%s", numero_normalizado.c_str());
    snprintf(botoes_controle[indice].status, tamanho_status_botao_controle, "%s", status_normalizado.c_str());
    // O servidor ja definiu portao e status. Um cadastro ou reset antigo nao pode ser enviado depois e desfazer o vinculo.
    botoes_controle[indice].operacao_api_pendente = 0;
    geracao_botoes_controle++;
    gravacao_botoes_controle_pendente = true;
    encontrou = true;
    break;
  }
  portEXIT_CRITICAL(&protecao_botoes_controle);
  return encontrou;
}

// Localiza o botão pela assinatura e diz se ele pode pulsar um portão desta placa.
ResultadoConsultaBotaoControle consultaBotaoControle(uint32_t assinatura) {
  ResultadoConsultaBotaoControle resultado;
  memset(&resultado, 0, sizeof(resultado));
  portENTER_CRITICAL(&protecao_botoes_controle);
  for (uint8_t indice = 0; indice < quantidade_botoes_controle; indice++) {
    if (!botoes_controle[indice].ocupado ||
        !assinaturaConfereComIdentificadorArmazenado(botoes_controle[indice].identificador, assinatura)) {
      continue;
    }
    resultado.cadastrado = true;
    strncpy(resultado.identificador, botoes_controle[indice].identificador, tamanho_identificador_botao_controle - 1);
    strncpy(resultado.numero_portao, botoes_controle[indice].numero_portao, tamanho_numero_portao_botao_controle - 1);
    strncpy(resultado.status, botoes_controle[indice].status, tamanho_status_botao_controle - 1);
    resultado.operacao_api_pendente = botoes_controle[indice].operacao_api_pendente;
    break;
  }
  portEXIT_CRITICAL(&protecao_botoes_controle);
  if (!resultado.cadastrado) {
    return resultado;
  }

  uint8_t numero_portao = (uint8_t)atoi(resultado.numero_portao);
  bool vinculado = strcmp(resultado.status, "vinculado") == 0;
  resultado.pode_pulsar = vinculado && numero_portao >= 1 && numero_portao <= 3 && portaoExisteNaPlaca(numero_portao);
  resultado.numero_portao_pulso = resultado.pode_pulsar ? numero_portao : 0;
  return resultado;
}

// Entrega uma vez o portão que o botão vinculado pediu para pulsar.
uint8_t consomeComandoBotaoControle(char *identificador_botao, char *numero_portao_texto) {
  uint8_t numero_portao = comando_botao_controle_portao_pendente;
  if (identificador_botao != nullptr) {
    identificador_botao[0] = '\0';
    if (numero_portao != 0) {
      strncpy(identificador_botao, identificador_comando_botao_controle_pendente, tamanho_identificador_botao_controle - 1);
      identificador_botao[tamanho_identificador_botao_controle - 1] = '\0';
    }
  }
  if (numero_portao_texto != nullptr) {
    numero_portao_texto[0] = '\0';
    if (numero_portao != 0) {
      strncpy(numero_portao_texto, numero_portao_comando_botao_controle_pendente, tamanho_numero_portao_botao_controle - 1);
      numero_portao_texto[tamanho_numero_portao_botao_controle - 1] = '\0';
    }
  }
  if (numero_portao != 0) {
    comando_botao_controle_portao_pendente = 0;
    identificador_comando_botao_controle_pendente[0] = '\0';
    numero_portao_comando_botao_controle_pendente[0] = '\0';
  }
  return numero_portao;
}

// Coloca na fila do loop o log de abertura ou fechamento feito pelo pulso do controle.
void enfileiraLogPulsoBotaoControle(const char *identificador_botao, const char *numero_portao_texto,
                                    bool comando_de_abertura) {
  if (fila_logs_pulsos_botoes_controle == nullptr) {
    print_log("ERRO: fila de logs de botoes de controle nao criada. Log descartado.");
    return;
  }
  LogPulsoBotaoControle log_pulso;
  memset(&log_pulso, 0, sizeof(log_pulso));
  if (identificador_botao != nullptr) {
    strncpy(log_pulso.identificador_botao, identificador_botao, tamanho_identificador_botao_controle - 1);
  }
  if (numero_portao_texto != nullptr) {
    strncpy(log_pulso.numero_portao, numero_portao_texto, tamanho_numero_portao_botao_controle - 1);
  }
  strncpy(log_pulso.tipo_evento, comando_de_abertura ? "gate_open" : "gate_close", sizeof(log_pulso.tipo_evento) - 1);
  if (xQueueSend(fila_logs_pulsos_botoes_controle, &log_pulso, 0) != pdTRUE) {
    print_log("ERRO: fila de logs de botoes de controle cheia. Log descartado.");
  }
}

// JSON do cadastro ou do reset. A placa não tem login_hash: identifica a placa pelo serial_id.
String montaCorpoJsonBotaoControle(const String &identificador, bool resetar) {
  String token = api_token;
  String serial = plaque_serial;
  token.replace("\\", "\\\\");
  token.replace("\"", "\\\"");
  serial.replace("\\", "\\\\");
  serial.replace("\"", "\\\"");
  String corpo = "{\"internal_token\":\"";
  corpo += token;
  corpo += "\",\"serial_id\":\"";
  corpo += serial;
  corpo += "\",\"button_id\":";
  corpo += identificador;
  if (resetar) {
    corpo += ",\"control_id\":0,\"port_number\":0";
  }
  corpo += "}";
  return corpo;
}

// Corpo do cadastro do botão: internal_token, serial_id e button_id numérico.
String montaCorpoCadastroBotaoControle(const String &identificador) {
  return montaCorpoJsonBotaoControle(identificador, false);
}

// Corpo do reset: solta o controle (control_id 0) e tira o portão (port_number 0).
String montaCorpoResetBotaoControle(const String &identificador) {
  return montaCorpoJsonBotaoControle(identificador, true);
}

// Corpo do log de abertura/fechamento por pulso de controle, com button_id e port_number do objeto encontrado.
String montaCorpoLogPulsoBotaoControle(const char *tipo_evento, const char *identificador, const char *numero_portao) {
  return "internal_token=" + api_token + "&serial=" + codificaTextoParaUrl(plaque_serial) +
         "&serial_id=" + codificaTextoParaUrl(plaque_serial) + "&event_type=" + String(tipo_evento) +
         "&button_id=" + codificaTextoParaUrl(identificador) + "&port_number=" + codificaTextoParaUrl(numero_portao);
}

// Envia cadastro ou reset pendente. Sem confirmação da API, repete a cada 5 s até o servidor aceitar.
void enviaCadastrosBotoesControlePendentes() {
  if (momento_proxima_tentativa_envio_cadastro_botao_controle != 0 &&
      (long)(millis() - momento_proxima_tentativa_envio_cadastro_botao_controle) < 0) {
    return;
  }

  char identificador[tamanho_identificador_botao_controle] = "";
  char operacao = 0;
  bool encontrou = false;
  portENTER_CRITICAL(&protecao_botoes_controle);
  for (uint8_t indice = 0; indice < quantidade_botoes_controle; indice++) {
    char pendencia = botoes_controle[indice].operacao_api_pendente;
    if (botoes_controle[indice].ocupado &&
        (pendencia == operacao_api_botao_cadastrar || pendencia == operacao_api_botao_resetar)) {
      strncpy(identificador, botoes_controle[indice].identificador, tamanho_identificador_botao_controle - 1);
      identificador[tamanho_identificador_botao_controle - 1] = '\0';
      operacao = pendencia;
      encontrou = true;
      break;
    }
  }
  portEXIT_CRITICAL(&protecao_botoes_controle);
  if (!encontrou) {
    return;
  }

  bool resetar = operacao == operacao_api_botao_resetar;
  const char *rota = resetar ? rota_atualizacao_botao_controle : rota_cadastro_botao_controle;
  String url = api_url_aplicativo + rota;
  String corpo = resetar ? montaCorpoResetBotaoControle(identificador) : montaCorpoCadastroBotaoControle(identificador);
  print_log("Enviando botao " + String(identificador) + " para " + String(rota) + ".");
  String resposta;
  bool enviou = executarPostHttp(rota, url, corpo, 10000, &resposta, tipo_conteudo_json);
  if (enviou && (resposta.indexOf("\"status\":400") >= 0 || resposta.indexOf("\"status\": 400") >= 0)) {
    enviou = false;
  }
  momento_proxima_tentativa_envio_cadastro_botao_controle =
      enviou ? 0 : millis() + intervalo_reenvio_cadastro_botao_controle_ms;

  bool confirmou_esta_operacao = false;
  if (enviou) {
    portENTER_CRITICAL(&protecao_botoes_controle);
    for (uint8_t indice = 0; indice < quantidade_botoes_controle; indice++) {
      if (!botoes_controle[indice].ocupado || strcmp(botoes_controle[indice].identificador, identificador) != 0) {
        continue;
      }
      if (botoes_controle[indice].operacao_api_pendente == operacao) {
        botoes_controle[indice].operacao_api_pendente = 0;
        geracao_botoes_controle++;
        gravacao_botoes_controle_pendente = true;
        confirmou_esta_operacao = true;
      }
      break;
    }
    portEXIT_CRITICAL(&protecao_botoes_controle);
  }

  if (confirmou_esta_operacao) {
    salvaBotoesControle();
    print_log((resetar ? String("Reset do botao ") : String("Cadastro do botao ")) + String(identificador) +
              " enviado para a API.");
  } else if (enviou) {
    print_log("Resposta da API chegou depois de um novo cadastro do botao " + String(identificador) +
              ". A operacao mais recente continua pendente.");
  } else {
    print_log("Falha ao enviar o botao " + String(identificador) + " para " + String(rota) +
              ". Nova tentativa em seguida.");
  }
}

// Envia os logs de pulso que a tarefa dos portões enfileirou. Se a rede falhar, devolve o item para a fila.
void enviaLogsPulsosBotoesControle() {
  if (fila_logs_pulsos_botoes_controle == nullptr || !redeProntaParaApi()) {
    return;
  }

  LogPulsoBotaoControle log_pulso;
  while (xQueueReceive(fila_logs_pulsos_botoes_controle, &log_pulso, 0) == pdTRUE) {
    String url = api_url_aplicativo + rota_log_pulso_botao_controle;
    String corpo = montaCorpoLogPulsoBotaoControle(log_pulso.tipo_evento, log_pulso.identificador_botao, log_pulso.numero_portao);
    if (!executarPostHttp(rota_log_pulso_botao_controle, url, corpo, 10000, nullptr)) {
      print_log("ERRO: log de pulso do botao " + String(log_pulso.identificador_botao) + " nao foi enviado.");
      xQueueSendToFront(fila_logs_pulsos_botoes_controle, &log_pulso, 0);
      return;
    }
    print_log("Log de " + String(log_pulso.tipo_evento) + " do botao " + String(log_pulso.identificador_botao) +
              " no portao " + String(log_pulso.numero_portao) + " enviado.");
  }
}

// Aceita status livre, vinculado ou desativado e portão 0, 1, 2 ou 3.
bool dadosAtualizacaoBotaoControleValidos(const String &identificador, const String &numero_portao, const String &status) {
  uint32_t assinatura = 0;
  if (!converteIdentificadorBotaoControle(identificador, assinatura)) {
    return false;
  }
  if (numero_portao != "0" && numero_portao != "1" && numero_portao != "2" && numero_portao != "3") {
    return false;
  }
  return status == "livre" || status == "vinculado" || status == "desativado";
}

// Lê button_id, port_number e status do action_data de update_button_control.
bool extraiAtualizacaoBotaoControleDoPayload(const String &payload, String &identificador, String &numero_portao,
                                             String &status) {
  String action_data = normalizaJsonBotaoControle(extraiValorActionData(payload));
  identificador = extraiCampoObjetoBotaoControle(action_data, "button_id");
  numero_portao = extraiCampoObjetoBotaoControle(action_data, "port_number");
  status = extraiCampoObjetoBotaoControle(action_data, "status");
  identificador.trim();
  numero_portao.trim();
  status.trim();
  status.toLowerCase();
  return identificador.length() > 0 && numero_portao.length() > 0 && status.length() > 0;
}

// Recoloca na lista em uso os botões que estão na Preferences e a memória não tem mais.
// Não apaga nem troca o portão e o status dos botões que já estão na memória.
void mesclaBotoesControleAusentesDasPreferences() {
  if (!preferences.begin("portguard", true)) {
    print_log("ERRO: nao foi possivel ler a Preferences para procurar os botoes.");
    return;
  }
  String json = preferences.getString(chave_armazenamento_botoes_controle, "[]");
  preferences.end();
  uint8_t acrescentados = interpretaJsonBotoesControle(json, false);
  if (acrescentados > 0) {
    print_log("Botoes encontrados so na Preferences e recolocados na lista em uso: " + String(acrescentados) + ".");
  }
}

// Aplica cada objeto de update_button_control do action_data. Um objeto ou um array inteiro.
// Não substitui a lista: só muda o botão cujo button_id já existe.
uint8_t aplicaAtualizacoesBotaoControleNoActionData(const String &action_data, ResultadoAtualizacaoBotaoControle *resultados,
                                                   uint8_t capacidade) {
  uint8_t quantidade = 0;
  int cursor = 0;
  while (cursor < (int)action_data.length() && quantidade < capacidade && resultados != nullptr) {
    int inicio = action_data.indexOf('{', cursor);
    if (inicio < 0) {
      break;
    }
    int fim = action_data.indexOf('}', inicio);
    if (fim < 0) {
      break;
    }
    String objeto = action_data.substring(inicio, fim + 1);
    cursor = fim + 1;

    ResultadoAtualizacaoBotaoControle &resultado = resultados[quantidade];
    memset(&resultado, 0, sizeof(resultado));
    String identificador = extraiCampoObjetoBotaoControle(objeto, "button_id");
    String numero_portao = extraiCampoObjetoBotaoControle(objeto, "port_number");
    String status = extraiCampoObjetoBotaoControle(objeto, "status");
    identificador.trim();
    numero_portao.trim();
    status.trim();
    status.toLowerCase();
    strncpy(resultado.identificador, identificador.c_str(), sizeof(resultado.identificador) - 1);
    strncpy(resultado.numero_portao, numero_portao.c_str(), sizeof(resultado.numero_portao) - 1);
    strncpy(resultado.status, status.c_str(), sizeof(resultado.status) - 1);

    resultado.dados_validos = dadosAtualizacaoBotaoControleValidos(identificador, numero_portao, status);
    if (!resultado.dados_validos) {
      quantidade++;
      continue;
    }

    uint32_t assinatura = 0;
    converteIdentificadorBotaoControle(identificador, assinatura);
    resultado.localizado = consultaBotaoControle(assinatura).cadastrado;
    resultado.atualizado_na_memoria = atualizaBotaoControleArmazenado(identificador, numero_portao, status);
    quantidade++;
  }
  return quantidade;
}

// Aplica update_button_control: acha cada botão, grava a lista inteira na Preferences e só então responde a fila.
bool processarAtualizacaoBotaoControle(long queue_id, const String &payload) {
  static long queue_id_gravacao_botao_em_retry = 0;
  static uint8_t tentativas_gravacao_deste_evento = 0;
  static ResultadoAtualizacaoBotaoControle resultados[quantidade_maxima_botoes_controle];

  String action_data_recebido = extraiValorActionData(payload);
  String action_data = normalizaJsonBotaoControle(action_data_recebido);
  print_log("");
  print_log("======== update_button_control ========");
  print_log("action_data recebido: " + (action_data_recebido.length() ? action_data_recebido : String("(vazio)")));
  if (action_data != action_data_recebido && action_data.length() > 0) {
    print_log("action_data interpretado: " + action_data);
  }

  if (queue_id <= 0) {
    print_log("Sem queue_id. Nenhum botao foi alterado.");
    print_log("========================================");
    return false;
  }

  bool aguardando_gravacao = momento_proxima_gravacao_botoes_controle != 0 &&
                             (long)(millis() - momento_proxima_gravacao_botoes_controle) < 0;
  if (queue_id == queue_id_gravacao_botao_em_retry && aguardando_gravacao) {
    print_log("A gravacao deste update_button_control ainda nao foi conferida na Preferences. Nova tentativa em seguida.");
    print_log("========================================");
    return false;
  }
  if (queue_id != queue_id_gravacao_botao_em_retry) {
    tentativas_gravacao_deste_evento = 0;
  }

  mesclaBotoesControleAusentesDasPreferences();
  uint8_t quantidade = aplicaAtualizacoesBotaoControleNoActionData(action_data, resultados, quantidade_maxima_botoes_controle);
  if (quantidade == 0) {
    print_log("Nenhum botao foi lido no action_data. Nada foi alterado.");
    print_log("========================================");
    ultimoQueueIdNotificado = queue_id;
    atualizarStatusFila(queue_id, "failed");
    return false;
  }

  bool algum_invalido = false;
  bool algum_ausente = false;
  bool algum_atualizado = false;
  for (uint8_t indice = 0; indice < quantidade; indice++) {
    if (resultados[indice].dados_validos && resultados[indice].localizado && resultados[indice].atualizado_na_memoria) {
      algum_atualizado = true;
    }
  }

  bool gravou = true;
  if (algum_atualizado) {
    gravou = salvaBotoesControle();
  }

  for (uint8_t indice = 0; indice < quantidade; indice++) {
    ResultadoAtualizacaoBotaoControle &resultado = resultados[indice];
    if (!resultado.dados_validos) {
      algum_invalido = true;
      print_log("Objeto invalido no action_data. Identificador lido: " +
                String(resultado.identificador[0] ? resultado.identificador : "(vazio)") + ", portao " +
                String(resultado.numero_portao[0] ? resultado.numero_portao : "(vazio)") + ", status " +
                String(resultado.status[0] ? resultado.status : "(vazio)") + ".");
      print_log("Localizado nas Preferences: NAO.");
      print_log("Atualizado nas Preferences: NAO.");
      print_log("Pode acionar a abertura do portao: NAO.");
      continue;
    }

    print_log("Botao " + String(resultado.identificador) + ":");
    if (!resultado.localizado || !resultado.atualizado_na_memoria) {
      algum_ausente = true;
      print_log("Localizado nas Preferences: NAO. Este botao nao esta cadastrado na placa.");
      print_log("Botoes cadastrados na placa: " + montaJsonBotoesControle());
      print_log("Atualizado nas Preferences: NAO.");
      print_log("Pode acionar a abertura do portao: NAO.");
      continue;
    }

    print_log("Localizado nas Preferences: SIM.");
    if (gravou) {
      print_log("Atualizado nas Preferences: SIM. Portao " + String(resultado.numero_portao) + ", status " +
                String(resultado.status) + ".");
    } else {
      print_log("Atualizado nas Preferences: NAO. A lista em uso mudou, mas a gravacao ainda nao foi conferida.");
    }

    uint32_t assinatura = 0;
    converteIdentificadorBotaoControle(resultado.identificador, assinatura);
    ResultadoConsultaBotaoControle consulta = consultaBotaoControle(assinatura);
    if (consulta.pode_pulsar) {
      print_log("Pode acionar a abertura do portao: SIM. Ao ser pressionado, envia o comando do portao " +
                String(consulta.numero_portao_pulso) +
                ". Se o portao estiver fechado, a abertura ainda passa pelo intertravamento.");
    } else {
      print_log("Pode acionar a abertura do portao: NAO. Status " + String(consulta.status) + ", portao " +
                String(consulta.numero_portao) + ".");
    }
  }

  if (algum_atualizado && !gravou) {
    queue_id_gravacao_botao_em_retry = queue_id;
    tentativas_gravacao_deste_evento++;
    if (tentativas_gravacao_deste_evento < limite_tentativas_gravacao_botao_controle) {
      print_log("A fila vai repetir este update_button_control ate a Preferences confirmar a gravacao.");
      print_log("========================================");
      return false;
    }
    print_log("A Preferences nao confirmou a gravacao depois de varias tentativas. Respondendo failed para a fila seguir.");
    print_log("========================================");
    ultimoQueueIdNotificado = queue_id;
    queue_id_gravacao_botao_em_retry = 0;
    tentativas_gravacao_deste_evento = 0;
    atualizarStatusFila(queue_id, "failed");
    return false;
  }

  bool evento_completo = algum_atualizado && !algum_invalido && !algum_ausente && gravou;
  print_log("========================================");
  ultimoQueueIdNotificado = queue_id;
  queue_id_gravacao_botao_em_retry = 0;
  tentativas_gravacao_deste_evento = 0;
  atualizarStatusFila(queue_id, evento_completo ? "completed" : "failed");
  return evento_completo;
}

// Trata a ação open_or_close_gate da fila: envia o comando para a tarefa dos portões (mesmo efeito da
// receptora: abre com autorização se o portão estiver fechado, fecha se estiver aberto, respeitando o
// intertravamento) e responde a fila com completed ou failed. O queue_id é marcado como processado antes de
// acionar o portão para que uma falha ao responder a API nunca faça o mesmo comando ser executado de novo.
bool processarAbrirOuFecharPortao(long queue_id, const String &payload) {
  uint8_t numero_portao = extraiNumeroPortaoDoActionData(payload);
  print_log("");
  print_log("Evento open_or_close_gate recebido. Portao solicitado: " +
            (numero_portao > 0 ? String(numero_portao) : String("invalido")));

  if (queue_id <= 0) {
    print_log("open_or_close_gate sem queue_id: comando nao executado para evitar acionamentos repetidos.");
    return false;
  }
  ultimoQueueIdNotificado = queue_id;

  if (numero_portao == 0 || !portaoExisteNaPlaca(numero_portao)) {
    print_log("open_or_close_gate com action_data invalido ou portao fora desta placa (use 1 a " +
              String(quantidade_portoes_placa) + ").");
    atualizarStatusFila(queue_id, "failed");
    return false;
  }

  bool comando_executado = false;
  if (!enviaComandoRemotoParaTarefaPortoes(numero_portao, comando_executado)) {
    print_log("ERRO: tarefa dos portoes nao respondeu ao open_or_close_gate.");
  } else if (!comando_executado) {
    print_log("open_or_close_gate bloqueado pelo intertravamento (outro portao esta aberto).");
  } else {
    print_log("open_or_close_gate executado no portao " + String(numero_portao) + ".");
  }

  atualizarStatusFila(queue_id, comando_executado ? "completed" : "failed");
  return comando_executado;
}

// Trata a ação down_siren da fila: pede à tarefa dos portões para desligar a sirene (encerra os alarmes ativos dos
// três portões) e responde completed ou failed. O queue_id é marcado como processado antes de executar.
bool processarDesligarSirene(long queue_id) {
  print_log("");
  print_log("Evento down_siren recebido: desligando a sirene.");

  if (queue_id <= 0) {
    print_log("down_siren sem queue_id: comando nao executado.");
    return false;
  }
  ultimoQueueIdNotificado = queue_id;

  bool comando_executado = false;
  if (!enviaComandoRemotoParaTarefaPortoes(comando_remoto_desligar_sirene, comando_executado)) {
    print_log("ERRO: tarefa dos portoes nao respondeu ao down_siren.");
    comando_executado = false;
  } else {
    print_log("down_siren executado: sirene desligada.");
  }

  atualizarStatusFila(queue_id, comando_executado ? "completed" : "failed");
  return comando_executado;
}

bool buscarConfigWifiNaApi() {
  String payload;
  if (!chamarApiListagem(&payload)) {
    return false;
  }

  String actionType = extrairValorJsonString(payload, "action_type");
  String ssid = extrairValorJsonString(payload, "wifi_ssid");
  String password = extrairValorJsonString(payload, "wifi_password");
  String idTexto = extrairValorJsonNumero(payload, "id");
  if (idTexto.length() == 0) {
    idTexto = extrairValorJsonString(payload, "id");
  }
  long queueId = idTexto.length() ? idTexto.toInt() : 0;

  // A fila é consultada a cada meio segundo: sem ação nova não há log.
  if (actionType.length() == 0 && ssid.length() == 0) {
    return false;
  }
  if (queueId > 0 && queueId == ultimoQueueIdNotificado) {
    if (queueId == queue_id_ultimo_status_respondido &&
        millis() - momento_ultimo_envio_status_fila >= intervalo_reenvio_status_fila_ms) {
      String status_reenviado = ultimo_status_respondido_fila;
      print_log("queue_id " + String(queueId) + " continua pendente na API. Reenviando status " + status_reenviado +
                " (a acao nao sera executada de novo).");
      atualizarStatusFila(queueId, status_reenviado.c_str());
    }
    return true;
  }
  if (wifiCredenciaisPendentes && queueId > 0 && queueId == queueIdPendente) {
    return true;
  }

  bool acao_comando_portoes = actionType == "open_or_close_gate" || actionType == "down_siren" ||
                              actionType == "intertravamento" || actionType.startsWith("fotocelula") ||
                              acaoEhManutencao(actionType) || actionType == "update_button_control";
  if (acao_comando_portoes) {
    String status_acao = extrairValorJsonString(payload, "action_status");
    if (status_acao.length() > 0 && status_acao != "pending") {
      ultimoQueueIdNotificado = queueId;
      return true;
    }
  }

  bool primeira_leitura_da_acao = queueId != ultimo_queue_id_exibido_no_log;
  ultimo_queue_id_exibido_no_log = queueId;
  if (primeira_leitura_da_acao) {
    Serial.print("action_type recebido: ");
    Serial.println(actionType.length() ? actionType : "(vazio)");
    Serial.print("wifi_ssid recebido: ");
    Serial.println(ssid.length() ? ssid : "(vazio)");
    Serial.print("wifi_password: ");
    Serial.println(password.length() ? "(preenchida)" : "(vazia)");
    Serial.print("queue_id recebido: ");
    Serial.println(queueId > 0 ? String(queueId) : "(vazio)");
    String criado_em = extrairValorJsonString(payload, "created_at");
    if (criado_em.length() > 0) {
      print_log("created_at: " + criado_em);
    }
  }

  if (actionType == "reset_plaque") {
    return processarResetPlaque(queueId);
  }

  bool acao_imediata = actionType == "open_or_close_gate" || actionType == "down_siren";
  bool relogio_sincronizado = (long long)time(nullptr) >= epoch_minimo_relogio_sincronizado;
  if (acao_imediata && !relogio_sincronizado &&
      millis() - momento_inicio_sincronizacao_relogio < tempo_maximo_espera_sincronizacao_relogio_ms) {
    if (primeira_leitura_da_acao) {
      print_log(actionType + " aguardando o relogio (NTP) para conferir se o comando nao e antigo.");
    }
    return false;
  }
  if (acao_imediata && queueId > 0 && acaoDaFilaExpirada(payload, (long long)time(nullptr))) {
    print_log(actionType + " (queue_id " + String(queueId) + ") criado ha mais de " +
              String((long)validade_acao_imediata_fila_segundos) + " s: comando antigo nao executado.");
    ultimoQueueIdNotificado = queueId;
    atualizarStatusFila(queueId, "failed");
    return false;
  }

  if (actionType == "open_or_close_gate") {
    return processarAbrirOuFecharPortao(queueId, payload);
  }

  if (actionType == "down_siren") {
    return processarDesligarSirene(queueId);
  }

  if (actionType == "intertravamento" || actionType.startsWith("fotocelula") || acaoEhManutencao(actionType)) {
    return processarAlteracaoFlagPortoes(queueId, actionType, payload);
  }

  if (actionType == "update_button_control") {
    return processarAtualizacaoBotaoControle(queueId, payload);
  }

  // Ação sem tratamento: responde failed, senão a API continua devolvendo esta ação e a fila trava nela.
  if (actionType.length() > 0 && actionType != "config_plaque") {
    print_log("Acao desconhecida ignorada: " + actionType + ". Respondendo failed para a fila seguir.");
    if (queueId > 0) {
      ultimoQueueIdNotificado = queueId;
      atualizarStatusFila(queueId, "failed");
    }
    return false;
  }

  if (ssid.length() == 0) {
    Serial.println("Nenhuma configuracao WiFi encontrada na API.");
    if (confirmaConfiguracaoPeloCabo(queueId)) {
      return true;
    }
    if (queueId > 0 && queueId != ultimoQueueIdNotificado) {
      ultimoQueueIdNotificado = queueId;
      atualizarStatusFila(queueId, statusDaConfiguracaoQuandoWifiNaoSobe(false));
    }
    return false;
  }

  if (ssid == wifiSsidRejeitado && password == wifiPasswordRejeitado) {
    Serial.println("Credenciais WiFi ja rejeitadas. Nao vou tentar de novo.");
    if (confirmaConfiguracaoPeloCabo(queueId)) {
      return true;
    }
    if (queueId > 0 && queueId != ultimoQueueIdNotificado) {
      atualizarStatusFila(queueId, statusDaConfiguracaoQuandoWifiNaoSobe(false));
      ultimoQueueIdNotificado = queueId;
    }
    return false;
  }

  bool mesmaConfig = wifi_ssid == ssid && wifi_password == password;
  if (mesmaConfig && setup_finished && !wifiCredenciaisPendentes) {
    if (primeira_leitura_da_acao) {
      Serial.println("Configuracao WiFi da API igual a persistida.");
    }

    if (queueId > 0 && queueId != ultimoQueueIdNotificado) {
      ultimoQueueIdNotificado = queueId;
      atualizarStatusFila(queueId, "completed");
    }

    return true;
  }

  Serial.println();
  Serial.println("Nova configuracao WiFi recebida da API (config_plaque).");
  Serial.print("SSID: ");
  Serial.println(ssid);

  wifi_ssid = ssid;
  wifi_password = password;
  wifiCredenciaisPendentes = true;
  wifiConfigFalhou = false;
  momentoInicioConfigWifi = millis();
  ultimaTentativaWifi = 0;
  // O cabo confirma a configuração na hora. A tentativa de Wi-Fi continua e, se conectar, segue persistida.
  confirmaConfiguracaoPeloCabo(queueId);
  queueIdPendente = queueId;

  if (wifiConectado()) {
    WiFi.disconnect(false, false);
    delay(100);
  }

  conectarWifi();
  return true;
}

void falharConfigWifiRecebida() {
  if (wifiConfigFalhou && !wifiCredenciaisPendentes) {
    return;
  }

  long queueId = queueIdPendente;
  wifiSsidRejeitado = wifi_ssid;
  wifiPasswordRejeitado = wifi_password;
  wifiConfigFalhou = true;
  wifiCredenciaisPendentes = false;
  momentoInicioConfigWifi = 0;

  Serial.println();
  Serial.println("Credenciais WiFi invalidas. PARANDO tentativas.");
  Serial.print("SSID rejeitado: ");
  Serial.println(wifiSsidRejeitado);

  if (queueId > 0 && !confirmaConfiguracaoPeloCabo(queueId) && queueId != ultimoQueueIdNotificado) {
    atualizarStatusFila(queueId, statusDaConfiguracaoQuandoWifiNaoSobe(false));
    ultimoQueueIdNotificado = queueId;
  }
  queueIdPendente = 0;

  // Com a Ethernet no ar, não desliga o rádio por completo: WiFi.mode(WIFI_OFF) derruba o cabo.
  if (ethIniciado) {
    WiFi.disconnect(false, false);
  } else {
    WiFi.disconnect(true, false);
  }
  delay(100);
  wifi_persistido_ja_iniciado = false;

  carregarConfigWifi();

  if (wifiEstaConfigurado() &&
      !(wifi_ssid == wifiSsidRejeitado && wifi_password == wifiPasswordRejeitado)) {
    Serial.print("Restaurando WiFi persistido: ");
    Serial.println(wifi_ssid);
    wifiConfigFalhou = false;
    ultimaTentativaWifi = 0;
    conectarWifi();
    return;
  }

  wifi_ssid = "";
  wifi_password = "";
  setup_finished = false;
  Serial.println("Sem WiFi valido. Voltando para cabo de rede.");
  if (!ethIniciado) {
    iniciarEthernet();
  }
  preferirCaboSeWifiCaiu();
}

// Credencial persistida e válida: o modem fica acordado e a pilha reconecta sozinha.
bool deveManterReconexaoAutomaticaWifi() {
  return setup_finished && wifi_ssid.length() > 0 && !wifiCredenciaisPendentes && !wifiConfigFalhou;
}

// Desliga o sleep do modem no ESP32-S3 e aplica auto-reconnect só quando as credenciais persistidas são válidas.
void configuraEstabilidadeWifiEsp32S3() {
  if (WiFi.getMode() == WIFI_MODE_NULL || WiFi.getMode() == WIFI_OFF) {
    return;
  }
  WiFi.setSleep(false);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(deveManterReconexaoAutomaticaWifi());
}

// Reaplica sleep desligado e auto-reconnect quando o rádio ganha ou perde o IP.
void onWifiEvent(arduino_event_id_t evento, arduino_event_info_t info) {
  (void)info;
  if (evento != ARDUINO_EVENT_WIFI_STA_GOT_IP && evento != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    return;
  }

  configuraEstabilidadeWifiEsp32S3();
  if (evento == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Network.setDefaultInterface(WiFi.STA);
    print_log("Wi-Fi conectado. Modem sem sleep e rota padrao na interface Wi-Fi.");
  }
}

// Registra o callback de Wi-Fi uma única vez, junto dos eventos da Ethernet.
void registraEventosWifiEsp32S3() {
  static bool eventos_wifi_registrados = false;
  if (eventos_wifi_registrados) {
    return;
  }
  eventos_wifi_registrados = true;
  Network.onEvent(onWifiEvent);
}

// Conecta o Wi-Fi persistido sem reiniciar o rádio a cada ciclo, ou faz uma tentativa única quando a fila pede config_plaque.
void conectarWifi() {
  if (wifiConfigFalhou || wifi_ssid.length() == 0) {
    return;
  }

  if (wifiConectado()) {
    wifi_persistido_ja_iniciado = true;
    configuraEstabilidadeWifiEsp32S3();
    Network.setDefaultInterface(WiFi.STA);
    cicloApiAssimQueRedeSubir = true;
    return;
  }

  /*
    Credenciais recém recebidas da API: uma única tentativa.
    Se falhar, marca a queue como error e não tenta de novo.
  */
  if (wifiCredenciaisPendentes) {
    if (ultimaTentativaWifi != 0) {
      wl_status_t statusWifi = WiFi.status();
      bool erroWifi = statusWifi == WL_CONNECT_FAILED ||
                      statusWifi == WL_NO_SSID_AVAIL;
      bool timeoutWifi = momentoInicioConfigWifi > 0 &&
                         millis() - momentoInicioConfigWifi >= TIMEOUT_CONFIG_WIFI_MS;

      if (erroWifi || timeoutWifi) {
        Serial.println();
        Serial.print("WiFi nao conectou. status=");
        Serial.println(statusWifiTexto());
        falharConfigWifiRecebida();
      }
      return;
    }

    ultimaTentativaWifi = millis();
    if (momentoInicioConfigWifi == 0) {
      momentoInicioConfigWifi = millis();
    }

    Serial.println();
    Serial.print("Tentativa UNICA de WiFi (queue): ");
    Serial.println(wifi_ssid);

    wifi_persistido_ja_iniciado = false;
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(false);
    // disconnect(false): desliga só a associação. disconnect(true) chama WiFi.mode(WIFI_OFF) e derruba o W5500.
    WiFi.disconnect(false, false);
    delay(100);
    WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());
    preferirCaboSeWifiCaiu();
    return;
  }

  if (!wifi_persistido_ja_iniciado) {
    wifi_persistido_ja_iniciado = true;
    ultimaTentativaWifi = millis();

    Serial.println();
    Serial.print("Conectando ao WiFi persistido: ");
    Serial.println(wifi_ssid);

    WiFi.mode(WIFI_STA);
    configuraEstabilidadeWifiEsp32S3();
    WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());
    preferirCaboSeWifiCaiu();
    return;
  }

  if (ultimaTentativaWifi != 0 &&
      millis() - ultimaTentativaWifi < INTERVALO_RECONECTAR_WIFI_MS) {
    return;
  }

  ultimaTentativaWifi = millis();
  configuraEstabilidadeWifiEsp32S3();
  WiFi.reconnect();
}

// ============================================================
// GERENCIAMENTO DAS REDES
// ============================================================

void verificarRede() {
  unsigned long agora = millis();

  if (!ethIniciado) {
    iniciarEthernet();
  }

  if (ethFlagPendenteSalvar) {
    ethFlagPendenteSalvar = false;
    salvarFlagEthernet(true);
  }

  if (!wifiConfigFalhou &&
      (wifiEstaConfigurado() || wifiCredenciaisPendentes) &&
      agora - ultimoValidarWifi >= INTERVALO_VALIDAR_WIFI_MS) {
    ultimoValidarWifi = agora;

    if (!wifiConectado()) {
      conectarWifi();
    } else if (wifiCredenciaisPendentes) {
      setup_finished = true;
      wifiCredenciaisPendentes = false;
      wifiConfigFalhou = false;
      wifi_persistido_ja_iniciado = true;
      configuraEstabilidadeWifiEsp32S3();
      salvarConfigWifi();
      Serial.println("WiFi conectado. Credenciais persistidas.");
      atualizarStatusFila(queueIdPendente, "completed");
      momentoInicioConfigWifi = 0;
    }
  }

  aplicarRotaDeRede();

  bool wifi_no_ar = wifiConectado();
  bool cabo_no_ar = caboConectado();
  wifi_conectado_para_led = wifi_no_ar;
  cabo_conectado_para_led = cabo_no_ar;

  if (!wifi_no_ar && !cabo_no_ar) {
    reportarErroRede("nao ha conexao com a internet.");
  }

  bool redeOk = wifi_no_ar || cabo_no_ar;
  // O SNTP só pode ser iniciado depois que alguma interface de rede subiu; ele mesmo repete as tentativas.
  if (redeOk && !sincronizacao_relogio_iniciada) {
    sincronizacao_relogio_iniciada = true;
    momento_inicio_sincronizacao_relogio = agora;
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    print_log("Sincronizacao do relogio (NTP) iniciada para validar a idade das acoes da fila.");
  }

  bool horaDoCiclo = agora - ultimoCicloApi >= INTERVALO_API_MS;
  if (redeOk && cicloApiAssimQueRedeSubir) {
    horaDoCiclo = true;
  }

  bool aguardando_apos_falha_consulta_fila =
      consulta_fila_falhou && agora - momento_ultima_falha_consulta_fila < intervalo_espera_apos_falha_consulta_fila_ms;
  if (redeOk && !aguardando_apos_falha_consulta_fila &&
      agora - momento_ultima_consulta_fila >= intervalo_consulta_fila_ms) {
    momento_ultima_consulta_fila = agora;
    buscarConfigWifiNaApi();
  }

  if (horaDoCiclo) {
    ultimoCicloApi = agora;
    cicloApiAssimQueRedeSubir = false;
    Serial.println();
    Serial.print("Ciclo API. Meio atual: ");
    Serial.println(redeAtual());
    if (!redeOk) {
      buscarConfigWifiNaApi();
    }
    enviarEventoLog("heartbeat");
  }
}

// ============================================================
// FILA DE EVENTOS
// ============================================================

// Envia para a API os eventos que a tarefa dos portões colocou na fila.
// Sem rede os eventos permanecem na fila até a conexão voltar.
void enviaEventosPendentes() {
  if (fila_eventos_pendentes == nullptr) {
    return;
  }

  EventoPendente evento;
  while (redeProntaParaApi() && xQueueReceive(fila_eventos_pendentes, &evento, 0) == pdTRUE) {
    if (!enviarEventoLog(evento.tipo_evento, evento.numero_portao)) {
      print_log("ERRO: evento " + String(evento.tipo_evento) + " do portao " +
                String(evento.numero_portao) + " nao foi enviado para a API.");
    }
  }
}

// ============================================================
// TESTES
// ============================================================

// Verifica o mapa da MAV1022: I2C, W5500 e RF433 sem conflito com USB/RTC, canais do PCA9534 e repouso das saídas.
bool teste_configuracao_pinos() {
  const uint8_t pinos_usados[] = {
      pino_i2c_sda, pino_i2c_scl, pino_receptor_rf433, pino_botao_cadastrar_controle,
      ETH_SPI_MOSI, ETH_SPI_MISO, ETH_SPI_SCK, ETH_PHY_CS, ETH_PHY_IRQ, ETH_PHY_RST,
      pino_led_wifi, pino_led_ethernet, pino_led_status_servidor};
  const uint8_t pinos_esperados[] = {8, 9, 48, 5, 11, 12, 13, 14, 10, 7, 3, 21, 47};
  const uint8_t pinos_reservados[] = {15, 16, 19, 20};
  const uint8_t canais_entrada[] = {
      canal_entrada_receptora_portao_1, canal_entrada_sensor_portao_1,
      canal_entrada_receptora_portao_2, canal_entrada_sensor_portao_2,
      canal_entrada_receptora_portao_3, canal_entrada_sensor_portao_3};
  const uint8_t canais_saida[] = {
      canal_saida_motor_portao_1, canal_saida_motor_portao_2, canal_saida_motor_portao_3,
      canal_saida_fotocelula_portao_1, canal_saida_fotocelula_portao_2, canal_saida_fotocelula_portao_3,
      canal_saida_sirene, canal_saida_central_alarme};

  if (endereco_pca9534_saidas != 0x20 || endereco_pca9534_entradas != 0x21) {
    print_log("teste_configuracao_pinos: endereco I2C do PCA9534 diferente de 0x20/0x21.");
    return false;
  }

  for (int indice = 0; indice < (int)(sizeof(pinos_usados) / sizeof(pinos_usados[0])); indice++) {
    if (pinos_usados[indice] != pinos_esperados[indice]) {
      print_log("teste_configuracao_pinos: GPIO" + String(pinos_usados[indice]) + " fora do mapa da MAV1022.");
      return false;
    }
    for (uint8_t pino_reservado : pinos_reservados) {
      if (pinos_usados[indice] == pino_reservado) {
        print_log("teste_configuracao_pinos: GPIO" + String(pinos_usados[indice]) + " e USB ou cristal do RTC.");
        return false;
      }
    }
    for (int indice_comparado = indice + 1; indice_comparado < (int)(sizeof(pinos_usados) / sizeof(pinos_usados[0]));
         indice_comparado++) {
      if (pinos_usados[indice] == pinos_usados[indice_comparado]) {
        print_log("teste_configuracao_pinos: GPIO" + String(pinos_usados[indice]) + " usado mais de uma vez.");
        return false;
      }
    }
  }

  for (int indice = 0; indice < 6; indice++) {
    if (canais_entrada[indice] != (uint8_t)(indice + 1)) {
      print_log("teste_configuracao_pinos: canal de entrada fora da ordem da architecture.md.");
      return false;
    }
  }
  for (int indice = 0; indice < 8; indice++) {
    if (canais_saida[indice] != (uint8_t)(indice + 1)) {
      print_log("teste_configuracao_pinos: canal de saida fora da ordem da architecture.md.");
      return false;
    }
  }

  if (canalEntradaAtivo(0x00, canal_entrada_sensor_portao_1) != true ||
      canalEntradaAtivo(0xFF, canal_entrada_receptora_portao_1) != false ||
      canalEntradaAtivo(byte_entradas_repouso_seguro_pca9534, canal_entrada_receptora_portao_1) != false ||
      canalEntradaAtivo(byte_entradas_repouso_seguro_pca9534, canal_entrada_sensor_portao_1) != true ||
      canalEntradaAtivo(byte_entradas_repouso_seguro_pca9534, canal_entrada_receptora_portao_2) != false ||
      canalEntradaAtivo(byte_entradas_repouso_seguro_pca9534, canal_entrada_sensor_portao_2) != true ||
      canalEntradaAtivo(byte_entradas_repouso_seguro_pca9534, canal_entrada_receptora_portao_3) != false ||
      canalEntradaAtivo(byte_entradas_repouso_seguro_pca9534, canal_entrada_sensor_portao_3) != true ||
      canalEntradaAtivo(0x08, canal_entrada_sensor_portao_2) != false ||
      canalEntradaAtivo(0x20, canal_entrada_sensor_portao_3) != false) {
    print_log("teste_configuracao_pinos: polaridade ativa-baixa das entradas incorreta.");
    return false;
  }

  EstadoPortao portao_1_repouso;
  EstadoPortao portao_2_repouso;
  iniciaEstadoPortao(portao_1_repouso, 1, false, true, 0, true);
  iniciaEstadoPortao(portao_2_repouso, 2, false, true, 0, true);
  NiveisSaidas niveis = calculaSaidas(portao_1_repouso, portao_2_repouso, true, true, 0);
  uint8_t byte_repouso = montaByteSaidasPca9534(niveis);

  return niveis.motor_portao_1 == LOW && niveis.motor_portao_2 == LOW && niveis.motor_portao_3 == LOW &&
         niveis.sirene == LOW && niveis.fotocelula_portao_1 == HIGH && niveis.fotocelula_portao_2 == HIGH &&
         niveis.fotocelula_portao_3 == HIGH && niveis.central_alarme == HIGH && byte_repouso == 0xB8;
}

// Verifica o filtro de estabilização: repiques menores que tempo_estabilizacao_entrada_ms são ignorados.
bool teste_estabilizacao_entrada() {
  EntradaEstabilizada entrada;
  iniciaEntradaEstabilizada(entrada, false, 0);

  if (atualizaEntradaEstabilizada(entrada, true, 10)) {
    return false;
  }
  if (atualizaEntradaEstabilizada(entrada, false, 20)) {
    return false;
  }
  atualizaEntradaEstabilizada(entrada, true, 30);
  if (atualizaEntradaEstabilizada(entrada, true, 30 + tempo_estabilizacao_entrada_ms - 1)) {
    return false;
  }
  return atualizaEntradaEstabilizada(entrada, true, 30 + tempo_estabilizacao_entrada_ms);
}

// Verifica o fluxo de comando autorizado: pulso no motor, abertura dentro da janela e nenhum alarme.
bool teste_comando_autorizado() {
  EstadoPortao portao;
  EstadoPortao outro_portao;
  iniciaEstadoPortao(portao, 1, false, true, 0, true);
  iniciaEstadoPortao(outro_portao, 2, false, true, 0, true);

  processaPortao(portao, true, true, 100);
  if (!portao.motor_acionado || !portao.abertura_autorizada) {
    return false;
  }
  if (calculaSaidas(portao, outro_portao, true, true, 100).motor_portao_1 != HIGH) {
    return false;
  }

  processaPortao(portao, false, true, 100 + tempo_pulso_motor_ms);
  if (portao.motor_acionado) {
    return false;
  }

  unsigned long momento_abertura = 100 + janela_comando_autorizado_ms - 1;
  processaPortao(portao, false, false, momento_abertura);
  if (!portao.portao_aberto || portao.abertura_autorizada || portao.alarme_abertura_indevida_ativo) {
    return false;
  }
  if (portao.eventos_registrados_teste.indexOf("gate_open") < 0 ||
      portao.eventos_registrados_teste.indexOf("gate_broken") >= 0) {
    return false;
  }

  NiveisSaidas niveis = calculaSaidas(portao, outro_portao, true, true, momento_abertura);
  return niveis.sirene == LOW && niveis.central_alarme == HIGH && niveis.motor_portao_1 == LOW;
}

// Verifica que comando com o portão já aberto aciona o motor (fechamento) sem abrir janela de autorização.
bool teste_comando_com_portao_aberto() {
  EstadoPortao portao;
  iniciaEstadoPortao(portao, 1, false, false, 0, true);

  processaPortao(portao, true, false, 100);
  if (!portao.motor_acionado || portao.abertura_autorizada) {
    return false;
  }

  processaPortao(portao, false, true, 100 + janela_comando_autorizado_ms + 1);
  return !portao.portao_aberto && portao.contador_possivel_falha_sensor == 0 &&
         portao.eventos_registrados_teste == "gate_close;";
}

// Verifica abertura sem comando: alarme, gate_broken, sirene contínua (SAÍDA 8 invertida) durante todo o
// tempo em que o portão fica aberto, sirene mantida por tempo_sirene_apos_fechamento_abertura_indevida_ms
// após fechar (inclusive quando o portão é fechado logo em seguida) e nova abertura indevida nesse intervalo.
bool teste_abertura_indevida() {
  EstadoPortao portao;
  EstadoPortao outro_portao;
  iniciaEstadoPortao(portao, 1, false, true, 0, true);
  iniciaEstadoPortao(outro_portao, 2, false, true, 0, true);

  processaPortao(portao, false, false, 100);
  if (!portao.alarme_abertura_indevida_ativo ||
      portao.eventos_registrados_teste.indexOf("gate_open") < 0 ||
      portao.eventos_registrados_teste.indexOf("gate_broken") < 0) {
    print_log("teste_abertura_indevida: abertura sem comando nao gerou alarme/gate_broken.");
    return false;
  }

  NiveisSaidas niveis = calculaSaidas(portao, outro_portao, true, true, 100);
  if (niveis.sirene != HIGH || niveis.central_alarme != LOW || niveis.motor_portao_1 != LOW) {
    print_log("teste_abertura_indevida: sirene nao ligou na abertura indevida.");
    return false;
  }

  unsigned long momento_fechamento = 100 + 300000;
  processaPortao(portao, false, false, momento_fechamento - 1);
  if (!portao.alarme_abertura_indevida_ativo ||
      calculaSaidas(portao, outro_portao, true, true, momento_fechamento - 1).sirene != HIGH) {
    print_log("teste_abertura_indevida: sirene parou com o portao ainda aberto indevidamente.");
    return false;
  }

  processaPortao(portao, false, true, momento_fechamento);
  if (portao.portao_aberto || portao.alarme_abertura_indevida_ativo || !portao.sirene_apos_fechamento_indevido_ativa ||
      portao.eventos_registrados_teste.indexOf("gate_close") < 0) {
    print_log("teste_abertura_indevida: fechamento da abertura indevida nao iniciou a sirene pos-fechamento.");
    return false;
  }

  unsigned long momento_fim_sirene = momento_fechamento + tempo_sirene_apos_fechamento_abertura_indevida_ms;
  processaPortao(portao, false, true, momento_fim_sirene - 1);
  niveis = calculaSaidas(portao, outro_portao, true, true, momento_fim_sirene - 1);
  if (niveis.sirene != HIGH || niveis.central_alarme != LOW) {
    print_log("teste_abertura_indevida: sirene parou antes do tempo apos o fechamento.");
    return false;
  }

  processaPortao(portao, false, true, momento_fim_sirene);
  niveis = calculaSaidas(portao, outro_portao, true, true, momento_fim_sirene);
  if (portao.sirene_apos_fechamento_indevido_ativa || niveis.sirene != LOW || niveis.central_alarme != HIGH) {
    print_log("teste_abertura_indevida: sirene nao desligou apos o tempo pos-fechamento.");
    return false;
  }

  iniciaEstadoPortao(portao, 1, false, true, 0, true);
  processaPortao(portao, false, false, 100);
  processaPortao(portao, false, true, 200);
  processaPortao(portao, false, true, 200 + tempo_sirene_apos_fechamento_abertura_indevida_ms / 2);
  if (calculaSaidas(portao, outro_portao, true, true, 200 + tempo_sirene_apos_fechamento_abertura_indevida_ms / 2).sirene != HIGH) {
    print_log("teste_abertura_indevida: sirene parou quando o portao foi fechado logo apos a abertura indevida.");
    return false;
  }

  unsigned long momento_nova_abertura = 200 + tempo_sirene_apos_fechamento_abertura_indevida_ms / 2;
  processaPortao(portao, false, false, momento_nova_abertura + 100);
  processaPortao(portao, false, false, 200 + tempo_sirene_apos_fechamento_abertura_indevida_ms + 100);
  if (!portao.alarme_abertura_indevida_ativo ||
      calculaSaidas(portao, outro_portao, true, true, 200 + tempo_sirene_apos_fechamento_abertura_indevida_ms + 100).sirene != HIGH) {
    print_log("teste_abertura_indevida: nova abertura indevida durante a sirene pos-fechamento nao manteve a sirene.");
    return false;
  }

  unsigned long momento_novo_fechamento = 200 + tempo_sirene_apos_fechamento_abertura_indevida_ms + 200;
  processaPortao(portao, false, true, momento_novo_fechamento);
  processaPortao(portao, false, true, momento_novo_fechamento + tempo_sirene_apos_fechamento_abertura_indevida_ms - 1);
  if (calculaSaidas(portao, outro_portao, true, true,
                    momento_novo_fechamento + tempo_sirene_apos_fechamento_abertura_indevida_ms - 1).sirene != HIGH) {
    print_log("teste_abertura_indevida: sirene pos-fechamento nao reiniciou apos nova abertura indevida.");
    return false;
  }
  processaPortao(portao, false, true, momento_novo_fechamento + tempo_sirene_apos_fechamento_abertura_indevida_ms);
  return calculaSaidas(portao, outro_portao, true, true,
                       momento_novo_fechamento + tempo_sirene_apos_fechamento_abertura_indevida_ms).sirene == LOW;
}

// Verifica o alerta de portão aberto por tempo excessivo: gate_timeout_open, sirene intermitente
// (ligada/desligada) e gate_timeout_close quando o portão fecha.
bool teste_portao_aberto_tempo_excessivo() {
  EstadoPortao portao;
  EstadoPortao outro_portao;
  iniciaEstadoPortao(portao, 1, false, true, 0, true);
  iniciaEstadoPortao(outro_portao, 2, false, true, 0, true);

  processaPortao(portao, true, true, 0);
  processaPortao(portao, false, false, 500);
  if (!portao.portao_aberto || portao.alarme_abertura_indevida_ativo) {
    return false;
  }

  processaPortao(portao, false, false, 500 + tempo_maximo_portao_aberto_ms - 1);
  if (portao.alerta_tempo_aberto_ativo) {
    return false;
  }

  unsigned long inicio_alerta = 500 + tempo_maximo_portao_aberto_ms;
  processaPortao(portao, false, false, inicio_alerta);
  if (!portao.alerta_tempo_aberto_ativo || portao.eventos_registrados_teste.indexOf("gate_timeout_open") < 0) {
    return false;
  }

  if (calculaSaidas(portao, outro_portao, true, true, inicio_alerta).sirene != HIGH) {
    return false;
  }
  NiveisSaidas niveis_pausa = calculaSaidas(portao, outro_portao, true, true,
                                            inicio_alerta + tempo_sirene_ligada_alerta_ms + tempo_sirene_desligada_alerta_ms / 2);
  if (niveis_pausa.sirene != LOW || niveis_pausa.central_alarme != HIGH) {
    return false;
  }
  if (calculaSaidas(portao, outro_portao, true, true,
                    inicio_alerta + tempo_sirene_ligada_alerta_ms + tempo_sirene_desligada_alerta_ms).sirene != HIGH) {
    return false;
  }

  processaPortao(portao, false, true, inicio_alerta + 5000);
  return !portao.alerta_tempo_aberto_ativo && !portao.portao_aberto &&
         portao.eventos_registrados_teste.indexOf("gate_timeout_close") >= 0;
}

// Conta quantas vezes um evento apareceu na lista gravada pelo modo de teste.
int contaEventoRegistradoNoTeste(const String &eventos, const char *tipo_evento) {
  int quantidade = 0;
  int posicao = eventos.indexOf(tipo_evento);
  while (posicao >= 0) {
    quantidade++;
    posicao = eventos.indexOf(tipo_evento, posicao + (int)strlen(tipo_evento));
  }
  return quantidade;
}

// Verifica o gate_timeout_open: sai aos 120 s, uma vez por abertura, com título e event_data {"port": N}.
// Aos 15 s não avisa. Cada portão conta o próprio tempo e a contagem zera quando o portão abre de novo.
bool teste_aviso_portao_aberto_sem_fechar() {
  if (tempo_maximo_portao_aberto_ms != 120000) {
    print_log("teste_aviso_portao_aberto_sem_fechar: gate_timeout_open nao esta em 120 segundos.");
    return false;
  }
  if (strcmp(titulo_aviso_portao_aberto_sem_fechar, "Portão aberto por mais de 2 minutos") != 0 ||
      strcmp(tipo_evento_portao_aberto_sem_fechar, "gate_timeout_open") != 0) {
    print_log("teste_aviso_portao_aberto_sem_fechar: titulo ou tipo do evento divergem do pedido.");
    return false;
  }
  EventoPendente evento_tamanho;
  if (strlen(tipo_evento_portao_aberto_sem_fechar) >= sizeof(evento_tamanho.tipo_evento)) {
    print_log("teste_aviso_portao_aberto_sem_fechar: gate_timeout_open nao cabe na fila de eventos.");
    return false;
  }
  if (montaEventDataPortaoAbertoSemFechar(1) != "{\"port\": 1}" ||
      montaEventDataPortaoAbertoSemFechar(2) != "{\"port\": 2}" ||
      montaEventDataPortaoAbertoSemFechar(3) != "{\"port\": 3}" ||
      codificaTextoParaUrl(montaEventDataPortaoAbertoSemFechar(1)) != "%7B%22port%22%3A%201%7D") {
    print_log("teste_aviso_portao_aberto_sem_fechar: event_data nao ficou no formato {\"port\": N}.");
    return false;
  }

  EstadoPortao portao;
  EstadoPortao outro_portao;
  iniciaEstadoPortao(portao, 1, false, true, 0, true);
  iniciaEstadoPortao(outro_portao, 2, false, true, 0, true);

  processaPortao(portao, true, true, 100);
  unsigned long momento_abertura = 200;
  processaPortao(portao, false, false, momento_abertura);
  if (!portao.portao_aberto || portao.momento_abertura != momento_abertura) {
    print_log("teste_aviso_portao_aberto_sem_fechar: a abertura pelo controle nao zerou o cronometro.");
    return false;
  }

  unsigned long momento_quinze_segundos = momento_abertura + 15000;
  processaPortao(portao, false, false, momento_quinze_segundos);
  if (contaEventoRegistradoNoTeste(portao.eventos_registrados_teste, tipo_evento_portao_aberto_sem_fechar) != 0 ||
      portao.alerta_tempo_aberto_ativo ||
      calculaSaidas(portao, outro_portao, true, true, momento_quinze_segundos).sirene != LOW) {
    print_log("teste_aviso_portao_aberto_sem_fechar: enviou gate_timeout_open aos 15 segundos.");
    return false;
  }

  unsigned long momento_antes = momento_abertura + tempo_maximo_portao_aberto_ms - 1;
  processaPortao(portao, false, false, momento_antes);
  if (contaEventoRegistradoNoTeste(portao.eventos_registrados_teste, tipo_evento_portao_aberto_sem_fechar) != 0 ||
      portao.alerta_tempo_aberto_ativo ||
      calculaSaidas(portao, outro_portao, true, true, momento_antes).sirene != LOW) {
    print_log("teste_aviso_portao_aberto_sem_fechar: avisou antes de 120 segundos.");
    return false;
  }

  unsigned long momento_limite = momento_abertura + tempo_maximo_portao_aberto_ms;
  processaPortao(portao, false, false, momento_limite);
  if (contaEventoRegistradoNoTeste(portao.eventos_registrados_teste, tipo_evento_portao_aberto_sem_fechar) != 1 ||
      !portao.alerta_tempo_aberto_ativo ||
      calculaSaidas(portao, outro_portao, true, true, momento_limite).sirene != HIGH) {
    print_log("teste_aviso_portao_aberto_sem_fechar: aos 120 segundos nao enviou gate_timeout_open uma unica vez.");
    return false;
  }

  processaPortao(portao, false, false, momento_limite + 5000);
  if (contaEventoRegistradoNoTeste(portao.eventos_registrados_teste, tipo_evento_portao_aberto_sem_fechar) != 1) {
    print_log("teste_aviso_portao_aberto_sem_fechar: repetiu gate_timeout_open na mesma abertura.");
    return false;
  }

  processaPortao(portao, false, true, momento_limite + 1000);
  if (portao.portao_aberto || portao.alerta_tempo_aberto_ativo) {
    print_log("teste_aviso_portao_aberto_sem_fechar: fechar o portao nao encerrou o alerta.");
    return false;
  }

  unsigned long momento_novo_comando = momento_limite + 2000;
  unsigned long momento_nova_abertura = momento_limite + 2500;
  processaPortao(portao, true, true, momento_novo_comando);
  processaPortao(portao, false, false, momento_nova_abertura);
  processaPortao(portao, false, false, momento_nova_abertura + tempo_maximo_portao_aberto_ms - 1);
  if (contaEventoRegistradoNoTeste(portao.eventos_registrados_teste, tipo_evento_portao_aberto_sem_fechar) != 1) {
    print_log("teste_aviso_portao_aberto_sem_fechar: a nova abertura nao zerou a contagem.");
    return false;
  }
  processaPortao(portao, false, false, momento_nova_abertura + tempo_maximo_portao_aberto_ms);
  if (contaEventoRegistradoNoTeste(portao.eventos_registrados_teste, tipo_evento_portao_aberto_sem_fechar) != 2 ||
      portao.numero_portao != 1) {
    print_log("teste_aviso_portao_aberto_sem_fechar: a segunda abertura do portao 1 nao avisou de novo.");
    return false;
  }

  EstadoPortao portao_fechado_no_limite;
  iniciaEstadoPortao(portao_fechado_no_limite, 1, false, true, 0, true);
  processaPortao(portao_fechado_no_limite, true, true, 100);
  processaPortao(portao_fechado_no_limite, false, false, 200);
  processaPortao(portao_fechado_no_limite, false, true, 200 + tempo_maximo_portao_aberto_ms);
  if (contaEventoRegistradoNoTeste(portao_fechado_no_limite.eventos_registrados_teste, tipo_evento_portao_aberto_sem_fechar) != 0) {
    print_log("teste_aviso_portao_aberto_sem_fechar: avisou mesmo com o portao fechando aos 120 segundos.");
    return false;
  }

  EstadoPortao portao_forcado;
  iniciaEstadoPortao(portao_forcado, 1, false, true, 0, true);
  processaPortao(portao_forcado, false, false, 100);
  processaPortao(portao_forcado, false, false, 100 + 15000);
  if (contaEventoRegistradoNoTeste(portao_forcado.eventos_registrados_teste, tipo_evento_portao_aberto_sem_fechar) != 0) {
    print_log("teste_aviso_portao_aberto_sem_fechar: abertura sem comando avisou aos 15 segundos.");
    return false;
  }
  processaPortao(portao_forcado, false, false, 100 + tempo_maximo_portao_aberto_ms);
  if (contaEventoRegistradoNoTeste(portao_forcado.eventos_registrados_teste, tipo_evento_portao_aberto_sem_fechar) != 1 ||
      portao_forcado.eventos_registrados_teste.indexOf("gate_broken") < 0) {
    print_log("teste_aviso_portao_aberto_sem_fechar: abertura sem comando nao enviou gate_timeout_open aos 120 segundos.");
    return false;
  }

  EstadoPortao portao_aplicativo;
  EstadoPortao portao_3;
  iniciaEstadoPortao(portao_aplicativo, 2, false, true, 0, true);
  iniciaEstadoPortao(portao_3, 3, false, true, 0, true);
  processaPortao(portao_aplicativo, false, true, 1000, false, true);
  processaPortao(portao_aplicativo, false, false, 1200);
  processaPortao(portao_3, true, true, 4000);
  processaPortao(portao_3, false, false, 5000);
  unsigned long momento_portao_2 = 1200 + tempo_maximo_portao_aberto_ms;
  processaPortao(portao_aplicativo, false, false, momento_portao_2);
  processaPortao(portao_3, false, false, momento_portao_2);
  if (contaEventoRegistradoNoTeste(portao_aplicativo.eventos_registrados_teste, tipo_evento_portao_aberto_sem_fechar) != 1 ||
      portao_aplicativo.numero_portao != 2 ||
      contaEventoRegistradoNoTeste(portao_3.eventos_registrados_teste, tipo_evento_portao_aberto_sem_fechar) != 0) {
    print_log("teste_aviso_portao_aberto_sem_fechar: o cronometro do aplicativo afetou o outro portao.");
    return false;
  }
  unsigned long momento_portao_3 = 5000 + tempo_maximo_portao_aberto_ms;
  processaPortao(portao_aplicativo, false, false, momento_portao_3);
  processaPortao(portao_3, false, false, momento_portao_3);
  if (contaEventoRegistradoNoTeste(portao_aplicativo.eventos_registrados_teste, tipo_evento_portao_aberto_sem_fechar) != 1 ||
      contaEventoRegistradoNoTeste(portao_3.eventos_registrados_teste, tipo_evento_portao_aberto_sem_fechar) != 1 ||
      portao_3.numero_portao != 3) {
    print_log("teste_aviso_portao_aberto_sem_fechar: o portao 3 nao avisou no proprio cronometro.");
    return false;
  }

  EstadoPortao portao_manutencao;
  iniciaEstadoPortao(portao_manutencao, 1, false, true, 0, true);
  processaPortao(portao_manutencao, true, true, 100, false, false, true);
  processaPortao(portao_manutencao, false, false, 200, false, false, true);
  processaPortao(portao_manutencao, false, false, 200 + 60000, false, false, true);
  if (contaEventoRegistradoNoTeste(portao_manutencao.eventos_registrados_teste, tipo_evento_portao_aberto_sem_fechar) != 0) {
    print_log("teste_aviso_portao_aberto_sem_fechar: avisou durante a manutencao.");
    return false;
  }
  unsigned long momento_saida_manutencao = 200 + 60000;
  processaPortao(portao_manutencao, false, false, momento_saida_manutencao + tempo_maximo_portao_aberto_ms - 1);
  if (contaEventoRegistradoNoTeste(portao_manutencao.eventos_registrados_teste, tipo_evento_portao_aberto_sem_fechar) != 0) {
    print_log("teste_aviso_portao_aberto_sem_fechar: o cronometro nao recomecou ao sair da manutencao.");
    return false;
  }
  processaPortao(portao_manutencao, false, false, momento_saida_manutencao + tempo_maximo_portao_aberto_ms);
  if (contaEventoRegistradoNoTeste(portao_manutencao.eventos_registrados_teste, tipo_evento_portao_aberto_sem_fechar) != 1) {
    print_log("teste_aviso_portao_aberto_sem_fechar: nao avisou 120 segundos depois de sair da manutencao.");
    return false;
  }

  EstadoPortao portao_estouro;
  unsigned long momento_abertura_estouro = 0xFFFFFFFFUL - 1000;
  iniciaEstadoPortao(portao_estouro, 1, false, true, momento_abertura_estouro - 200, true);
  processaPortao(portao_estouro, true, true, momento_abertura_estouro - 100);
  processaPortao(portao_estouro, false, false, momento_abertura_estouro);
  processaPortao(portao_estouro, false, false, momento_abertura_estouro + tempo_maximo_portao_aberto_ms - 1);
  if (contaEventoRegistradoNoTeste(portao_estouro.eventos_registrados_teste, tipo_evento_portao_aberto_sem_fechar) != 0) {
    print_log("teste_aviso_portao_aberto_sem_fechar: avisou antes da hora no estouro do millis().");
    return false;
  }
  processaPortao(portao_estouro, false, false, momento_abertura_estouro + tempo_maximo_portao_aberto_ms);
  if (contaEventoRegistradoNoTeste(portao_estouro.eventos_registrados_teste, tipo_evento_portao_aberto_sem_fechar) != 1) {
    print_log("teste_aviso_portao_aberto_sem_fechar: nao avisou no estouro do millis().");
    return false;
  }

  return true;
}

// Verifica a validade da janela de autorização: abertura após a janela gera alarme e
// expirações seguidas sem abertura geram need_maintenance no limite configurado.
bool teste_validade_comando_autorizado() {
  EstadoPortao portao;
  iniciaEstadoPortao(portao, 1, false, true, 0, true);

  processaPortao(portao, true, true, 100);
  processaPortao(portao, false, false, 100 + janela_comando_autorizado_ms + 1);
  if (!portao.alarme_abertura_indevida_ativo || portao.eventos_registrados_teste.indexOf("gate_broken") < 0) {
    return false;
  }

  iniciaEstadoPortao(portao, 1, false, true, 0, true);
  for (int tentativa = 0; tentativa < limite_possivel_falha_sensor; tentativa++) {
    unsigned long momento_comando = 1000 + tentativa * 10000UL;
    processaPortao(portao, true, true, momento_comando);
    processaPortao(portao, false, true, momento_comando + janela_comando_autorizado_ms + 1);
    if (portao.abertura_autorizada) {
      return false;
    }
  }

  return portao.contador_possivel_falha_sensor == 0 &&
         portao.eventos_registrados_teste == "plaque_problem;" &&
         textoFalhaSensorFimDeCurso(1) == "Falha de leitura no sensor de fim de curso do portão 1" &&
         textoFalhaSensorFimDeCurso(portao.numero_portao).indexOf("portão 1") >= 0;
}

// Verifica os 5 s a mais da janela: abertura lenta ainda é autorizada, e só o portão comandado
// dispara a sirene se o sensor abrir depois dos 13 s.
bool teste_janela_abertura_comando_estendida() {
  if (acrescimo_janela_abertura_comando_ms != 5000 || janela_comando_autorizado_ms != 13000) {
    print_log("teste_janela_abertura_comando_estendida: janela diferente de 13 segundos.");
    return false;
  }

  EstadoPortao portao;
  EstadoPortao outro_portao;
  iniciaEstadoPortao(portao, 1, false, true, 0, true);
  iniciaEstadoPortao(outro_portao, 2, false, true, 0, true);

  processaPortao(portao, true, true, 100);
  processaPortao(portao, false, true, 100 + 8000);
  if (!portao.abertura_autorizada || portao.alarme_abertura_indevida_ativo) {
    print_log("teste_janela_abertura_comando_estendida: os 8 s antigos ainda geraram arrombamento.");
    return false;
  }

  unsigned long momento_abertura_lenta = 100 + 8000 + 1;
  processaPortao(portao, false, false, momento_abertura_lenta);
  if (portao.alarme_abertura_indevida_ativo ||
      portao.eventos_registrados_teste.indexOf("gate_broken") >= 0 ||
      calculaSaidas(portao, outro_portao, true, true, momento_abertura_lenta).sirene != LOW) {
    print_log("teste_janela_abertura_comando_estendida: abertura dentro dos 13 s disparou a sirene.");
    return false;
  }

  iniciaEstadoPortao(portao, 1, false, true, 0, true);
  iniciaEstadoPortao(outro_portao, 2, false, true, 0, true);
  processaPortao(portao, false, true, 100, false, true);
  unsigned long momento_fora_da_janela = 100 + janela_comando_autorizado_ms + 1;
  processaPortao(portao, false, false, momento_fora_da_janela);
  if (!portao.alarme_abertura_indevida_ativo ||
      portao.eventos_registrados_teste.indexOf("gate_broken") < 0 ||
      calculaSaidas(portao, outro_portao, true, true, momento_fora_da_janela).sirene != HIGH) {
    print_log("teste_janela_abertura_comando_estendida: sensor apos 13 s nao disparou a sirene.");
    return false;
  }
  if (outro_portao.alarme_abertura_indevida_ativo || outro_portao.abertura_autorizada) {
    print_log("teste_janela_abertura_comando_estendida: o portao sem comando entrou na janela.");
    return false;
  }

  processaPortao(outro_portao, false, false, momento_fora_da_janela);
  if (!outro_portao.alarme_abertura_indevida_ativo ||
      calculaSaidas(portao, outro_portao, true, true, momento_fora_da_janela).sirene != HIGH) {
    print_log("teste_janela_abertura_comando_estendida: portao sem comando nao manteve o arrombamento.");
    return false;
  }

  return true;
}

// Verifica que os portões 1 e 2 são independentes: comando do portão 2 só aciona a SAÍDA 2 e
// abertura indevida do portão 1 não afeta a autorização do portão 2.
bool teste_independencia_portoes() {
  EstadoPortao portao_1;
  EstadoPortao portao_2;
  iniciaEstadoPortao(portao_1, 1, false, true, 0, true);
  iniciaEstadoPortao(portao_2, 2, false, true, 0, true);

  processaPortao(portao_1, false, true, 100);
  processaPortao(portao_2, true, true, 100);
  NiveisSaidas niveis = calculaSaidas(portao_1, portao_2, true, true, 100);
  if (niveis.motor_portao_1 != LOW || niveis.motor_portao_2 != HIGH) {
    return false;
  }

  processaPortao(portao_1, false, false, 200);
  processaPortao(portao_2, false, false, 200);
  if (!portao_1.alarme_abertura_indevida_ativo || portao_2.alarme_abertura_indevida_ativo) {
    return false;
  }
  if (portao_2.eventos_registrados_teste.indexOf("gate_broken") >= 0) {
    return false;
  }

  niveis = calculaSaidas(portao_1, portao_2, true, true, 200);
  return niveis.sirene == HIGH && niveis.central_alarme == LOW;
}

// Verifica o controle das fotocélulas (SAÍDA 4 = portão 1, SAÍDA 5 = portão 2, SAÍDA 6 = portão 3): cada uma é
// desligável independentemente das outras e sem afetar a sirene. Restaura as flags originais ao final.
bool teste_alimentacao_fotocelula() {
  EstadoPortao portao_1;
  EstadoPortao portao_2;
  iniciaEstadoPortao(portao_1, 1, false, true, 0, true);
  iniciaEstadoPortao(portao_2, 2, false, true, 0, true);

  NiveisSaidas niveis = calculaSaidas(portao_1, portao_2, false, true, 0);
  if (niveis.fotocelula_portao_1 != LOW || niveis.fotocelula_portao_2 != HIGH || niveis.sirene != LOW) {
    return false;
  }
  niveis = calculaSaidas(portao_1, portao_2, true, false, 0);
  if (niveis.fotocelula_portao_1 != HIGH || niveis.fotocelula_portao_2 != LOW || niveis.sirene != LOW) {
    return false;
  }

  bool fotocelula_portao_1_original = fotocelula_portao_1_alimentada;
  bool fotocelula_portao_2_original = fotocelula_portao_2_alimentada;
  bool fotocelula_portao_3_original = fotocelula_portao_3_alimentada;
  fotocelula_portao_1_alimentada = true;
  fotocelula_portao_2_alimentada = true;
  fotocelula_portao_3_alimentada = true;

  defineAlimentacaoFotocelula(2, false);
  bool desligou_somente_portao_2 = fotocelula_portao_1_alimentada && !fotocelula_portao_2_alimentada &&
                                   fotocelula_portao_3_alimentada;
  defineAlimentacaoFotocelula(2, true);
  defineAlimentacaoFotocelula(1, false);
  bool desligou_somente_portao_1 = !fotocelula_portao_1_alimentada && fotocelula_portao_2_alimentada &&
                                   fotocelula_portao_3_alimentada;
  defineAlimentacaoFotocelula(1, true);
  defineAlimentacaoFotocelula(3, false);
  bool desligou_somente_portao_3 = fotocelula_portao_1_alimentada && fotocelula_portao_2_alimentada &&
                                   !fotocelula_portao_3_alimentada;
  defineAlimentacaoFotocelula(3, true);
  niveis = calculaSaidas(portao_1, portao_2, fotocelula_portao_1_alimentada, fotocelula_portao_2_alimentada, 0, false,
                         nullptr, fotocelula_portao_3_alimentada);
  bool religou = niveis.fotocelula_portao_1 == HIGH && niveis.fotocelula_portao_2 == HIGH &&
                 niveis.fotocelula_portao_3 == HIGH && niveis.sirene == LOW;

  fotocelula_portao_1_alimentada = fotocelula_portao_1_original;
  fotocelula_portao_2_alimentada = fotocelula_portao_2_original;
  fotocelula_portao_3_alimentada = fotocelula_portao_3_original;
  return desligou_somente_portao_2 && desligou_somente_portao_1 && desligou_somente_portao_3 && religou;
}

// Verifica que as SAÍDAS 4, 5 e 6 (fotocélulas) ficam sempre em HIGH, independente de sirene e sensores, e que a
// SAÍDA 8 (central de alarme) é sempre o inverso da SAÍDA 7 (sirene), em repouso, abertura indevida,
// os portões em alarme, sirene pós-fechamento, sirene intermitente (ligada e em pausa). Também garante o
// mapeamento dos canais 4, 5, 6, 7 e 8 no byte do PCA9534.
bool teste_saida_fotocelula_e_central_alarme() {
  if (canal_saida_motor_portao_3 != 3 || canal_saida_fotocelula_portao_1 != 4 || canal_saida_fotocelula_portao_2 != 5 ||
      canal_saida_fotocelula_portao_3 != 6 || canal_saida_sirene != 7 || canal_saida_central_alarme != 8) {
    print_log("teste_saida_fotocelula_e_central_alarme: canais das SAIDAS 3 a 8 divergem da architecture.md.");
    return false;
  }

  EstadoPortao portao_1;
  EstadoPortao portao_2;
  iniciaEstadoPortao(portao_1, 1, false, true, 0, true);
  iniciaEstadoPortao(portao_2, 2, false, true, 0, true);

  NiveisSaidas niveis_cenarios[8];
  int quantidade_cenarios = 0;
  niveis_cenarios[quantidade_cenarios++] = calculaSaidas(portao_1, portao_2, true, true, 0);

  processaPortao(portao_1, false, false, 100);
  niveis_cenarios[quantidade_cenarios++] = calculaSaidas(portao_1, portao_2, true, true, 100);

  processaPortao(portao_2, false, false, 200);
  niveis_cenarios[quantidade_cenarios++] = calculaSaidas(portao_1, portao_2, true, true, 200);

  processaPortao(portao_1, false, true, 300);
  processaPortao(portao_2, false, true, 300);
  niveis_cenarios[quantidade_cenarios++] = calculaSaidas(portao_1, portao_2, true, true, 300);
  if (niveis_cenarios[1].sirene != HIGH || niveis_cenarios[2].sirene != HIGH || niveis_cenarios[3].sirene != HIGH) {
    print_log("teste_saida_fotocelula_e_central_alarme: sirene nao ligou na abertura indevida.");
    return false;
  }

  unsigned long fim_sirene_pos_fechamento = 300 + tempo_sirene_apos_fechamento_abertura_indevida_ms;
  processaPortao(portao_1, false, true, fim_sirene_pos_fechamento);
  processaPortao(portao_2, false, true, fim_sirene_pos_fechamento);
  niveis_cenarios[quantidade_cenarios++] = calculaSaidas(portao_1, portao_2, true, true, fim_sirene_pos_fechamento);
  if (niveis_cenarios[4].sirene != LOW) {
    print_log("teste_saida_fotocelula_e_central_alarme: sirene nao desligou apos o tempo pos-fechamento.");
    return false;
  }

  unsigned long momento_comando = fim_sirene_pos_fechamento + 1000;
  processaPortao(portao_1, true, true, momento_comando);
  processaPortao(portao_1, false, false, momento_comando + 500);
  unsigned long inicio_alerta = momento_comando + 500 + tempo_maximo_portao_aberto_ms;
  processaPortao(portao_1, false, false, inicio_alerta);
  niveis_cenarios[quantidade_cenarios++] = calculaSaidas(portao_1, portao_2, true, true, inicio_alerta);
  niveis_cenarios[quantidade_cenarios++] = calculaSaidas(portao_1, portao_2, true, true,
                                                         inicio_alerta + tempo_sirene_ligada_alerta_ms);
  if (niveis_cenarios[5].sirene != HIGH || niveis_cenarios[6].sirene != LOW) {
    print_log("teste_saida_fotocelula_e_central_alarme: sirene intermitente incorreta.");
    return false;
  }

  processaPortao(portao_1, false, true, inicio_alerta + 5000);
  niveis_cenarios[quantidade_cenarios++] = calculaSaidas(portao_1, portao_2, true, true, inicio_alerta + 5000);

  for (int indice = 0; indice < quantidade_cenarios; indice++) {
    const NiveisSaidas &niveis = niveis_cenarios[indice];
    if (niveis.fotocelula_portao_1 != HIGH || niveis.fotocelula_portao_2 != HIGH || niveis.fotocelula_portao_3 != HIGH) {
      print_log("teste_saida_fotocelula_e_central_alarme: SAIDA 4, 5 ou 6 em LOW no cenario " + String(indice) + ".");
      return false;
    }
    if (niveis.central_alarme == niveis.sirene) {
      print_log("teste_saida_fotocelula_e_central_alarme: SAIDA 8 nao e o inverso da SAIDA 7 no cenario " +
                String(indice) + ".");
      return false;
    }
    uint8_t byte_saidas = montaByteSaidasPca9534(niveis);
    bool bit_sirene = (byte_saidas & (uint8_t)(1 << (canal_saida_sirene - 1))) != 0;
    bool bit_central = (byte_saidas & (uint8_t)(1 << (canal_saida_central_alarme - 1))) != 0;
    bool bit_fotocelula_1 = (byte_saidas & (uint8_t)(1 << (canal_saida_fotocelula_portao_1 - 1))) != 0;
    bool bit_fotocelula_2 = (byte_saidas & (uint8_t)(1 << (canal_saida_fotocelula_portao_2 - 1))) != 0;
    bool bit_fotocelula_3 = (byte_saidas & (uint8_t)(1 << (canal_saida_fotocelula_portao_3 - 1))) != 0;
    if (bit_sirene != (niveis.sirene == HIGH) || bit_central != (niveis.central_alarme == HIGH) ||
        bit_fotocelula_1 != (niveis.fotocelula_portao_1 == HIGH) || bit_fotocelula_2 != (niveis.fotocelula_portao_2 == HIGH) ||
        bit_fotocelula_3 != (niveis.fotocelula_portao_3 == HIGH)) {
      print_log("teste_saida_fotocelula_e_central_alarme: byte do PCA9534 nao segue os niveis no cenario " +
                String(indice) + ".");
      return false;
    }
  }
  return true;
}

// Verifica o intertravamento: com o outro portão aberto o comando de abertura é ignorado (sem pulso e sem
// autorização, e abrir mesmo assim gera gate_broken); comando de fechamento nunca é bloqueado; com o outro
// portão fechado ou com intertravamento_ativo = false o comando de abertura funciona normalmente.
bool teste_intertravamento() {
  bool intertravamento_original = intertravamento_ativo;
  intertravamento_ativo = true;
  bool resultado = false;

  EstadoPortao portao_1;
  EstadoPortao portao_2;
  iniciaEstadoPortao(portao_1, 1, false, true, 0, true);
  iniciaEstadoPortao(portao_2, 2, false, true, 0, true);

  do {
    processaPortao(portao_1, true, true, 100, false);
    processaPortao(portao_1, false, false, 200, false);
    if (!portao_1.portao_aberto || portao_1.alarme_abertura_indevida_ativo) {
      print_log("teste_intertravamento: portao 1 nao abriu com o portao 2 fechado.");
      break;
    }
    processaPortao(portao_2, true, true, 300, portao_1.portao_aberto);
    if (portao_2.motor_acionado || portao_2.abertura_autorizada ||
        calculaSaidas(portao_1, portao_2, true, true, 300).motor_portao_2 != LOW) {
      print_log("teste_intertravamento: comando de abertura do portao 2 nao foi ignorado com o portao 1 aberto.");
      break;
    }

    iniciaEstadoPortao(portao_1, 1, false, true, 0, true);
    iniciaEstadoPortao(portao_2, 2, false, true, 0, true);
    processaPortao(portao_2, false, false, 100, false);
    if (!portao_2.alarme_abertura_indevida_ativo || portao_2.eventos_registrados_teste.indexOf("gate_broken") < 0 ||
        calculaSaidas(portao_1, portao_2, true, true, 100).sirene != HIGH ||
        calculaSaidas(portao_1, portao_2, true, true, 100).central_alarme != LOW) {
      print_log("teste_intertravamento: arrombamento do portao 2 nao gerou gate_broken nem sirene.");
      break;
    }

    iniciaEstadoPortao(portao_1, 1, false, true, 0, true);
    iniciaEstadoPortao(portao_2, 2, false, true, 0, true);
    processaPortao(portao_2, true, true, 100, portao_1.portao_aberto);
    processaPortao(portao_2, false, false, 600, portao_1.portao_aberto);
    if (!portao_2.portao_aberto || portao_2.alarme_abertura_indevida_ativo) {
      print_log("teste_intertravamento: portao 2 nao abriu com o portao 1 fechado.");
      break;
    }

    processaPortao(portao_1, true, true, 700, portao_2.portao_aberto);
    if (portao_1.motor_acionado || portao_1.abertura_autorizada ||
        calculaSaidas(portao_1, portao_2, true, true, 700).motor_portao_1 != LOW) {
      print_log("teste_intertravamento: comando de abertura do portao 1 nao foi ignorado com o portao 2 aberto.");
      break;
    }
    processaPortao(portao_1, false, false, 800, portao_2.portao_aberto);
    if (!portao_1.alarme_abertura_indevida_ativo || portao_1.eventos_registrados_teste.indexOf("gate_broken") < 0) {
      print_log("teste_intertravamento: abertura apos comando ignorado nao foi tratada como indevida.");
      break;
    }

    processaPortao(portao_1, true, false, 900, portao_2.portao_aberto);
    if (!portao_1.motor_acionado || portao_1.abertura_autorizada) {
      print_log("teste_intertravamento: comando de fechamento do portao 1 foi bloqueado.");
      break;
    }
    processaPortao(portao_2, true, false, 900, portao_1.portao_aberto);
    if (!portao_2.motor_acionado || portao_2.abertura_autorizada) {
      print_log("teste_intertravamento: comando de fechamento do portao 2 foi bloqueado.");
      break;
    }

    iniciaEstadoPortao(portao_1, 1, false, true, 0, true);
    processaPortao(portao_1, true, true, 100, false);
    if (!portao_1.motor_acionado || !portao_1.abertura_autorizada) {
      print_log("teste_intertravamento: comando de abertura bloqueado com o outro portao fechado.");
      break;
    }

    intertravamento_ativo = false;
    iniciaEstadoPortao(portao_1, 1, false, true, 0, true);
    processaPortao(portao_1, true, true, 100, true);
    if (!portao_1.motor_acionado || !portao_1.abertura_autorizada) {
      print_log("teste_intertravamento: comando de abertura bloqueado com intertravamento desativado.");
      break;
    }

    resultado = true;
  } while (false);

  intertravamento_ativo = intertravamento_original;
  return resultado;
}

// Verifica o intertravamento com três portões: um portão aberto bloqueia a abertura dos outros dois;
// fechamento continua liberado; abertura forçada depois do bloqueio gera gate_broken e liga a sirene
// (SAÍDA 7) com a central (SAÍDA 8) em LOW. Restaura intertravamento_ativo ao final.
bool teste_intertravamento_tres_portoes() {
  bool intertravamento_original = intertravamento_ativo;
  intertravamento_ativo = true;
  bool resultado = false;

  EstadoPortao portao_1;
  EstadoPortao portao_2;
  EstadoPortao portao_3;
  iniciaEstadoPortao(portao_1, 1, false, true, 0, true);
  iniciaEstadoPortao(portao_2, 2, false, true, 0, true);
  iniciaEstadoPortao(portao_3, 3, false, true, 0, true);

  do {
    processaPortao(portao_3, true, true, 100, false);
    processaPortao(portao_3, false, false, 200, false);
    if (!portao_3.portao_aberto || portao_3.alarme_abertura_indevida_ativo) {
      print_log("teste_intertravamento_tres_portoes: portao 3 nao abriu com os outros fechados.");
      break;
    }

    processaPortao(portao_1, true, true, 300, true);
    processaPortao(portao_2, true, true, 400, true);
    if (portao_1.motor_acionado || portao_1.abertura_autorizada || portao_2.motor_acionado ||
        portao_2.abertura_autorizada) {
      print_log("teste_intertravamento_tres_portoes: abertura do portao 1 ou 2 nao foi bloqueada com o portao 3 aberto.");
      break;
    }

    intertravamento_ativo = false;
    iniciaEstadoPortao(portao_1, 1, false, true, 0, true);
    processaPortao(portao_1, true, true, 500, true);
    processaPortao(portao_1, false, false, 600, true);
    intertravamento_ativo = true;
    processaPortao(portao_1, true, false, 700, true);
    if (!portao_1.motor_acionado) {
      print_log("teste_intertravamento_tres_portoes: fechamento do portao 1 foi bloqueado com o portao 3 aberto.");
      break;
    }

    iniciaEstadoPortao(portao_3, 3, false, true, 0, true);
    processaPortao(portao_3, true, true, 800, true);
    if (portao_3.motor_acionado || portao_3.abertura_autorizada) {
      print_log("teste_intertravamento_tres_portoes: abertura do portao 3 nao foi bloqueada com o portao 1 aberto.");
      break;
    }
    processaPortao(portao_3, false, false, 900, true);
    NiveisSaidas niveis = calculaSaidas(portao_1, portao_2, true, true, 900, false, &portao_3, true, 3);
    uint8_t byte_arrombamento_portao_3 = montaByteSaidasPca9534(niveis);
    bool sirene_ligada_portao_3 = (byte_arrombamento_portao_3 & (uint8_t)(1 << (canal_saida_sirene - 1))) != 0;
    bool rele_alarme_desligado = (byte_arrombamento_portao_3 & (uint8_t)(1 << (canal_saida_central_alarme - 1))) == 0;
    if (!portao_3.alarme_abertura_indevida_ativo || portao_3.eventos_registrados_teste.indexOf("gate_broken") < 0 ||
        niveis.sirene != HIGH || niveis.central_alarme != LOW || !sirene_ligada_portao_3 || !rele_alarme_desligado) {
      print_log("teste_intertravamento_tres_portoes: arrombamento do portao 3 nao ligou a sirene nem desligou o rele 8.");
      break;
    }

    iniciaEstadoPortao(portao_1, 1, false, true, 0, true);
    iniciaEstadoPortao(portao_2, 2, false, true, 0, true);
    iniciaEstadoPortao(portao_3, 3, false, true, 0, true);
    processaPortao(portao_2, true, true, 1000, false);
    processaPortao(portao_2, false, false, 1100, false);
    processaPortao(portao_3, true, true, 1200, true);
    if (!portao_2.portao_aberto || portao_3.motor_acionado || portao_3.abertura_autorizada) {
      print_log("teste_intertravamento_tres_portoes: portao 2 aberto nao bloqueou a abertura do portao 3.");
      break;
    }

    resultado = true;
  } while (false);

  intertravamento_ativo = intertravamento_original;
  return resultado;
}

// Verifica o open_or_close_gate: leitura do action_data em todos os formatos aceitos (vazio/null/1 = portão 1,
// 2 = portão 2, 3 = portão 3, outros = inválido) e o comando remoto no processaPortao (abre com autorização sem gerar
// gate_broken, fecha portão aberto e é bloqueado pelo intertravamento com o outro portão aberto).
bool teste_comando_remoto_portao() {
  struct CasoActionData {
    const char *payload;
    uint8_t numero_portao_esperado;
  };
  const CasoActionData casos_action_data[] = {
      {"{\"list\":[{\"id\":12,\"action_type\":\"open_or_close_gate\",\"action_data\":\"\"}]}", 1},
      {"{\"list\":[{\"id\":12,\"action_type\":\"open_or_close_gate\",\"action_data\":null}]}", 1},
      {"{\"list\":[{\"id\":12,\"action_type\":\"open_or_close_gate\"}]}", 1},
      {"{\"list\":[{\"id\":12,\"action_type\":\"open_or_close_gate\",\"action_data\":\"1\"}]}", 1},
      {"{\"list\":[{\"id\":12,\"action_type\":\"open_or_close_gate\",\"action_data\":\"2\"}]}", 2},
      {"{\"list\":[{\"id\":12,\"action_type\":\"open_or_close_gate\",\"action_data\": 2}]}", 2},
      {"{\"list\":[{\"id\":12,\"action_type\":\"open_or_close_gate\",\"action_data\":\"{\\\"port\\\":2}\"}]}", 2},
      {"{\"list\":[{\"id\":12,\"action_type\":\"open_or_close_gate\",\"action_data\":{\"port\":1}}]}", 1},
      {"{\"list\":[{\"id\":12,\"action_type\":\"open_or_close_gate\",\"action_data\":\"3\"}]}", 3},
      {"{\"list\":[{\"id\":12,\"action_type\":\"open_or_close_gate\",\"action_data\":\"4\"}]}", 0},
      {"{\"list\":[{\"id\":12,\"action_type\":\"open_or_close_gate\",\"action_data\":\"abc\"}]}", 0},
  };
  for (const CasoActionData &caso : casos_action_data) {
    uint8_t numero_portao = extraiNumeroPortaoDoActionData(caso.payload);
    if (numero_portao != caso.numero_portao_esperado) {
      print_log("teste_comando_remoto_portao: action_data leu portao " + String(numero_portao) + " em vez de " +
                String(caso.numero_portao_esperado) + " no payload " + String(caso.payload));
      return false;
    }
  }

  bool intertravamento_original = intertravamento_ativo;
  intertravamento_ativo = true;
  bool resultado = false;

  EstadoPortao portao_1;
  EstadoPortao portao_2;
  iniciaEstadoPortao(portao_1, 1, false, true, 0, true);
  iniciaEstadoPortao(portao_2, 2, false, true, 0, true);

  do {
    if (!processaPortao(portao_2, false, true, 100, false, true) || !portao_2.motor_acionado ||
        !portao_2.abertura_autorizada || calculaSaidas(portao_1, portao_2, true, true, 100).motor_portao_2 != HIGH) {
      print_log("teste_comando_remoto_portao: comando remoto nao acionou a abertura do portao 2.");
      break;
    }
    processaPortao(portao_2, false, false, 600);
    if (!portao_2.portao_aberto || portao_2.alarme_abertura_indevida_ativo ||
        portao_2.eventos_registrados_teste.indexOf("gate_broken") >= 0) {
      print_log("teste_comando_remoto_portao: abertura via comando remoto foi tratada como indevida.");
      break;
    }

    if (processaPortao(portao_1, false, true, 700, portao_2.portao_aberto, true) || portao_1.motor_acionado ||
        portao_1.abertura_autorizada) {
      print_log("teste_comando_remoto_portao: comando remoto abriu o portao 1 com o portao 2 aberto.");
      break;
    }

    processaPortao(portao_2, false, false, 600 + tempo_pulso_motor_ms);
    if (!processaPortao(portao_2, false, false, 2000, portao_1.portao_aberto, true) || !portao_2.motor_acionado ||
        portao_2.abertura_autorizada) {
      print_log("teste_comando_remoto_portao: comando remoto nao acionou o fechamento do portao 2.");
      break;
    }

    EstadoPortao portao_3;
    iniciaEstadoPortao(portao_3, 3, false, true, 0, true);
    if (!processaPortao(portao_3, false, true, 3000, false, true) || !portao_3.motor_acionado ||
        !portao_3.abertura_autorizada) {
      print_log("teste_comando_remoto_portao: comando do portao 3 nao liberou a abertura.");
      break;
    }
    processaPortao(portao_3, false, false, 3200, false, false);
    processaPortao(portao_3, false, true, 3400, false, false);
    processaPortao(portao_3, false, false, 3000 + tempo_pulso_motor_ms + 500, false, false);
    if (portao_3.alarme_abertura_indevida_ativo || portao_3.eventos_registrados_teste.indexOf("gate_broken") >= 0) {
      print_log("teste_comando_remoto_portao: abertura do sensor do portao 3 apos o comando gerou arrombamento.");
      break;
    }

    resultado = true;
  } while (false);

  intertravamento_ativo = intertravamento_original;
  return resultado;
}

// Verifica as ações intertravamento e fotocelula: leitura da flag do action_data (0/1, true/false, texto,
// número ou JSON, inclusive {"port":N,"value":V}; vazio e outros valores são inválidos), leitura do portão da
// fotocelula (sem port = 1; port 1 ou 2; outro = inválido), efeito de cada flag (intertravamento bloqueia ou não
// a abertura com o outro portão aberto; fotocelula 0/1 coloca só a SAÍDA do portão indicado em LOW/HIGH) e
// persistência na Preferences (salva e recarrega os mesmos valores). Restaura as flags originais ao final.
bool teste_comando_flags_placa() {
  struct CasoFlag {
    const char *payload;
    int flag_esperada;
  };
  const CasoFlag casos_flag[] = {
      {"{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":\"0\"}]}", 0},
      {"{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":\"1\"}]}", 1},
      {"{\"list\":[{\"id\":7,\"action_type\":\"intertravamento\",\"action_data\": 0}]}", 0},
      {"{\"list\":[{\"id\":7,\"action_type\":\"intertravamento\",\"action_data\":1}]}", 1},
      {"{\"list\":[{\"id\":7,\"action_type\":\"intertravamento\",\"action_data\":\"false\"}]}", 0},
      {"{\"list\":[{\"id\":7,\"action_type\":\"intertravamento\",\"action_data\":true}]}", 1},
      {"{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":\"{\\\"value\\\":0}\"}]}", 0},
      {"{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":\"\"}]}", -1},
      {"{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":null}]}", -1},
      {"{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\"}]}", -1},
      {"{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":\"2\"}]}", -1},
      {"{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":\"abc\"}]}", -1},
      {"{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":\"{\\\"port\\\":2,\\\"value\\\":0}\"}]}", 0},
      {"{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":{\"port\":2,\"value\":1}}]}", 1},
      {"{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":{\"value\":\"0\",\"port\":1}}]}", 0},
      {"{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":{\"port\":1,\"value\":false}}]}", 0},
      {"{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":{\"port\":2}}]}", -1},
  };
  for (const CasoFlag &caso : casos_flag) {
    int flag = extraiFlagDoActionData(caso.payload);
    if (flag != caso.flag_esperada) {
      print_log("teste_comando_flags_placa: action_data leu flag " + String(flag) + " em vez de " +
                String(caso.flag_esperada) + " no payload " + String(caso.payload));
      return false;
    }
  }

  struct CasoPortaoFotocelula {
    const char *tipo_acao;
    const char *payload;
    uint8_t numero_portao_esperado;
  };
  const CasoPortaoFotocelula casos_portao_fotocelula[] = {
      {"fotocelula_1", "{\"list\":[{\"id\":7,\"action_type\":\"fotocelula_1\",\"action_data\":\"0\"}]}", 1},
      {"fotocelula_2", "{\"list\":[{\"id\":7,\"action_type\":\"fotocelula_2\",\"action_data\":\"0\"}]}", 2},
      {"fotocelula_2", "{\"list\":[{\"id\":7,\"action_type\":\"fotocelula_2\",\"action_data\":\"1\"}]}", 2},
      {"fotocelula_3", "{\"list\":[{\"id\":7,\"action_type\":\"fotocelula_3\",\"action_data\":\"0\"}]}", 3},
      {"fotocelula_3", "{\"list\":[{\"id\":7,\"action_type\":\"fotocelula_3\",\"action_data\":\"1\"}]}", 3},
      {"fotocelula_port_3", "{\"list\":[{\"id\":7,\"action_type\":\"fotocelula_port_3\",\"action_data\":\"1\"}]}", 3},
      {"fotocelula", "{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":\"1\",\"port\":3}]}", 3},
      {"fotocelula", "{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":\"0\"}]}", 1},
      {"fotocelula", "{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\"}]}", 1},
      {"fotocelula", "{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":{\"port\":1,\"value\":0}}]}", 1},
      {"fotocelula", "{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":{\"port\":2,\"value\":0}}]}", 2},
      {"fotocelula", "{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":\"{\\\"port\\\":\\\"2\\\",\\\"value\\\":1}\"}]}", 2},
      {"fotocelula", "{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":{\"value\":1,\"port\":2}}]}", 2},
      {"fotocelula", "{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":{\"port\":3,\"value\":0}}]}", 3},
      {"fotocelula", "{\"list\":[{\"id\":7,\"action_type\":\"fotocelula\",\"action_data\":{\"port\":4,\"value\":0}}]}", 0},
  };
  for (const CasoPortaoFotocelula &caso : casos_portao_fotocelula) {
    uint8_t numero_portao = extraiPortaoFotocelula(caso.tipo_acao, caso.payload);
    if (numero_portao != caso.numero_portao_esperado) {
      print_log("teste_comando_flags_placa: fotocelula leu portao " + String(numero_portao) + " em vez de " +
                String(caso.numero_portao_esperado) + " no payload " + String(caso.payload));
      return false;
    }
  }

  bool intertravamento_original = intertravamento_ativo;
  bool fotocelula_portao_1_original = fotocelula_portao_1_alimentada;
  bool fotocelula_portao_2_original = fotocelula_portao_2_alimentada;
  bool fotocelula_portao_3_original = fotocelula_portao_3_alimentada;
  bool resultado = false;

  EstadoPortao portao_1;
  EstadoPortao portao_2;

  do {
    defineIntertravamento(false);
    iniciaEstadoPortao(portao_1, 1, false, true, 0, true);
    if (!processaPortao(portao_1, true, true, 100, true) || !portao_1.motor_acionado) {
      print_log("teste_comando_flags_placa: intertravamento = 0 continuou bloqueando a abertura.");
      break;
    }

    defineIntertravamento(true);
    iniciaEstadoPortao(portao_1, 1, false, true, 0, true);
    if (processaPortao(portao_1, true, true, 100, true) || portao_1.motor_acionado) {
      print_log("teste_comando_flags_placa: intertravamento = 1 nao bloqueou a abertura.");
      break;
    }

    iniciaEstadoPortao(portao_1, 1, false, true, 0, true);
    iniciaEstadoPortao(portao_2, 2, false, true, 0, true);
    fotocelula_portao_1_alimentada = true;
    fotocelula_portao_2_alimentada = true;
    defineAlimentacaoFotocelula(2, false);
    NiveisSaidas niveis = calculaSaidas(portao_1, portao_2, fotocelula_portao_1_alimentada,
                                        fotocelula_portao_2_alimentada, 0);
    if (niveis.fotocelula_portao_2 != LOW || niveis.fotocelula_portao_1 != HIGH) {
      print_log("teste_comando_flags_placa: fotocelula port 2 = 0 nao colocou somente a SAIDA 5 em LOW.");
      break;
    }
    defineAlimentacaoFotocelula(1, false);
    defineAlimentacaoFotocelula(2, true);
    niveis = calculaSaidas(portao_1, portao_2, fotocelula_portao_1_alimentada, fotocelula_portao_2_alimentada, 0);
    if (niveis.fotocelula_portao_1 != LOW || niveis.fotocelula_portao_2 != HIGH) {
      print_log("teste_comando_flags_placa: fotocelula port 1 = 0 nao colocou somente a SAIDA 4 em LOW.");
      break;
    }
    defineAlimentacaoFotocelula(1, true);
    defineAlimentacaoFotocelula(3, false);
    niveis = calculaSaidas(portao_1, portao_2, fotocelula_portao_1_alimentada, fotocelula_portao_2_alimentada, 0, false,
                           nullptr, fotocelula_portao_3_alimentada);
    if (niveis.fotocelula_portao_1 != HIGH || niveis.fotocelula_portao_2 != HIGH || niveis.fotocelula_portao_3 != LOW) {
      print_log("teste_comando_flags_placa: fotocelula do portao 3 = 0 nao colocou somente a SAIDA 6 em LOW.");
      break;
    }
    defineAlimentacaoFotocelula(3, true);

    intertravamento_ativo = intertravamento_original;
    fotocelula_portao_1_alimentada = fotocelula_portao_1_original;
    fotocelula_portao_2_alimentada = fotocelula_portao_2_original;
    fotocelula_portao_3_alimentada = fotocelula_portao_3_original;
    salvaConfiguracaoPortoes();
    intertravamento_ativo = !intertravamento_original;
    fotocelula_portao_1_alimentada = !fotocelula_portao_1_original;
    fotocelula_portao_2_alimentada = !fotocelula_portao_2_original;
    fotocelula_portao_3_alimentada = !fotocelula_portao_3_original;
    carregaConfiguracaoPortoes();
    if (intertravamento_ativo != intertravamento_original ||
        fotocelula_portao_1_alimentada != fotocelula_portao_1_original ||
        fotocelula_portao_2_alimentada != fotocelula_portao_2_original ||
        fotocelula_portao_3_alimentada != fotocelula_portao_3_original) {
      print_log("teste_comando_flags_placa: flags nao foram persistidas/recarregadas da Preferences.");
      break;
    }

    resultado = true;
  } while (false);

  intertravamento_ativo = intertravamento_original;
  fotocelula_portao_1_alimentada = fotocelula_portao_1_original;
  fotocelula_portao_2_alimentada = fotocelula_portao_2_original;
  fotocelula_portao_3_alimentada = fotocelula_portao_3_original;
  return resultado;
}

// Verifica o modo manutenção: sirene sempre desligada (SAÍDA 8 em HIGH) mesmo com alarme ativo, abertura sem
// autorização não arma alarme nem gera gate_broken, alarmes ativos são encerrados e não voltam ao sair da manutenção,
// tempo aberto recomeça a contar ao sair, comandos continuam funcionando, eventos não entram na fila da API,
// enviarEventoLog não envia nada e a flag é persistida na Preferences. Restaura o estado original ao final.
bool teste_modo_manutencao() {
  bool manutencao_portao_1_original = manutencao_portao_1;
  bool manutencao_portao_2_original = manutencao_portao_2;
  bool manutencao_portao_3_original = manutencao_portao_3;
  bool resultado = false;

  EstadoPortao portao;
  EstadoPortao outro_portao;
  iniciaEstadoPortao(portao, 1, false, true, 0, true);
  iniciaEstadoPortao(outro_portao, 2, false, true, 0, true);

  do {
    processaPortao(portao, false, false, 100, false, false, true);
    NiveisSaidas niveis = calculaSaidas(portao, outro_portao, true, true, 100, true);
    if (portao.alarme_abertura_indevida_ativo || portao.eventos_registrados_teste.indexOf("gate_broken") >= 0 ||
        niveis.sirene != LOW || niveis.central_alarme != HIGH) {
      print_log("teste_modo_manutencao: abertura sem autorizacao armou alarme ou ligou a sirene.");
      break;
    }

    processaPortao(portao, false, false, 100 + 300000, false, false, true);
    if (portao.alerta_tempo_aberto_ativo) {
      print_log("teste_modo_manutencao: alerta de tempo aberto disparou durante a manutencao.");
      break;
    }
    unsigned long momento_saida_manutencao = 100 + 300000;
    processaPortao(portao, false, false, momento_saida_manutencao + tempo_maximo_portao_aberto_ms - 1);
    if (portao.alerta_tempo_aberto_ativo || calculaSaidas(portao, outro_portao, true, true,
                                                          momento_saida_manutencao + tempo_maximo_portao_aberto_ms - 1).sirene != LOW) {
      print_log("teste_modo_manutencao: tempo aberto nao recomecou a contar ao sair da manutencao.");
      break;
    }
    processaPortao(portao, false, false, momento_saida_manutencao + tempo_maximo_portao_aberto_ms);
    if (!portao.alerta_tempo_aberto_ativo) {
      print_log("teste_modo_manutencao: alerta de tempo aberto nao voltou a funcionar apos a manutencao.");
      break;
    }

    iniciaEstadoPortao(portao, 1, false, true, 0, true);
    processaPortao(portao, false, false, 100);
    if (!portao.alarme_abertura_indevida_ativo ||
        calculaSaidas(portao, outro_portao, true, true, 100).sirene != HIGH) {
      print_log("teste_modo_manutencao: alarme normal nao armou fora da manutencao.");
      break;
    }
    niveis = calculaSaidas(portao, outro_portao, true, true, 100, true);
    if (niveis.sirene != LOW || niveis.central_alarme != HIGH) {
      print_log("teste_modo_manutencao: sirene tocou com alarme ativo durante a manutencao.");
      break;
    }
    processaPortao(portao, false, false, 200, false, false, true);
    processaPortao(portao, false, true, 300, false, false, true);
    processaPortao(portao, false, true, 400);
    if (portao.alarme_abertura_indevida_ativo || portao.sirene_apos_fechamento_indevido_ativa ||
        calculaSaidas(portao, outro_portao, true, true, 400).sirene != LOW) {
      print_log("teste_modo_manutencao: alarme ativo voltou a tocar apos a manutencao.");
      break;
    }

    iniciaEstadoPortao(portao, 1, false, true, 0, true);
    if (!processaPortao(portao, true, true, 100, false, false, true) || !portao.motor_acionado ||
        !portao.abertura_autorizada) {
      print_log("teste_modo_manutencao: comando do portao nao funcionou durante a manutencao.");
      break;
    }

    defineManutencaoPortao(1, true);
    if (fila_eventos_pendentes != nullptr) {
      UBaseType_t eventos_antes = uxQueueMessagesWaiting(fila_eventos_pendentes);
      EstadoPortao portao_real;
      iniciaEstadoPortao(portao_real, 1, false, true, 0, false);
      registraEventoPortao(portao_real, "gate_open");
      if (uxQueueMessagesWaiting(fila_eventos_pendentes) != eventos_antes) {
        xQueueReset(fila_eventos_pendentes);
        print_log("teste_modo_manutencao: evento do portao 1 entrou na fila durante a manutencao.");
        break;
      }
      EstadoPortao portao_2_real;
      iniciaEstadoPortao(portao_2_real, 2, false, true, 0, false);
      registraEventoPortao(portao_2_real, "gate_open");
      if (uxQueueMessagesWaiting(fila_eventos_pendentes) != eventos_antes + 1) {
        xQueueReset(fila_eventos_pendentes);
        print_log("teste_modo_manutencao: manutencao do portao 1 bloqueou evento do portao 2.");
        break;
      }
      xQueueReset(fila_eventos_pendentes);
    }
    if (!enviarEventoLog("gate_open", 1)) {
      print_log("teste_modo_manutencao: evento do portao em manutencao nao foi descartado.");
      break;
    }

    salvaConfiguracaoPortoes();
    manutencao_portao_1 = false;
    carregaConfiguracaoPortoes();
    if (!manutencao_portao_1) {
      print_log("teste_modo_manutencao: manutencao do portao 1 nao foi persistida na Preferences.");
      break;
    }

    resultado = true;
  } while (false);

  manutencao_portao_1 = manutencao_portao_1_original;
  manutencao_portao_2 = manutencao_portao_2_original;
  manutencao_portao_3 = manutencao_portao_3_original;
  salvaConfiguracaoPortoes();
  return resultado;
}

// Verifica a quantidade de portões do serial, o intertravamento só entre portões existentes e fora de
// manutenção, e o relé 8 abrindo (SAÍDA 8 em LOW) quando a sirene dispara.
bool teste_portoes_da_placa_e_rele_alarme() {
  uint8_t quantidade_original = quantidade_portoes_placa;
  bool manutencao_portao_2_original = manutencao_portao_2;
  bool intertravamento_original = intertravamento_ativo;
  bool resultado = false;

  do {
    if (quantidadePortoesDoSerial("1port") != 1 || quantidadePortoesDoSerial("2port") != 2 ||
        quantidadePortoesDoSerial("3port") != 3 || quantidadePortoesDoSerial(plaque_serial) != 3 ||
        quantidade_portoes_configurada != 3) {
      print_log("teste_portoes_da_placa_e_rele_alarme: serial da API desligou o portao 3.");
      break;
    }

    quantidade_portoes_placa = 3;
    EstadoPortao portao_3;
    iniciaEstadoPortao(portao_3, 3, false, true, 0, true);
    processaPortao(portao_3, true, true, 100, false);
    if (!portao_3.motor_acionado) {
      print_log("teste_portoes_da_placa_e_rele_alarme: comando do portao 3 nao atracou o rele.");
      break;
    }
    iniciaEstadoPortao(portao_3, 3, false, true, 0, true);
    processaPortao(portao_3, false, false, 200, false);
    EstadoPortao portao_1_fechado;
    EstadoPortao portao_2_fechado;
    iniciaEstadoPortao(portao_1_fechado, 1, false, true, 0, true);
    iniciaEstadoPortao(portao_2_fechado, 2, false, true, 0, true);
    NiveisSaidas niveis_portao_3 = calculaSaidas(portao_1_fechado, portao_2_fechado, true, true, 200, false, &portao_3, true, 3);
    uint8_t byte_portao_3 = montaByteSaidasPca9534(niveis_portao_3);
    bool sirene_do_portao_3 = (byte_portao_3 & (uint8_t)(1 << (canal_saida_sirene - 1))) != 0;
    bool rele_8_desligado_no_portao_3 = (byte_portao_3 & (uint8_t)(1 << (canal_saida_central_alarme - 1))) == 0;
    if (!portao_3.alarme_abertura_indevida_ativo || portao_3.eventos_registrados_teste.indexOf("gate_broken") < 0 ||
        niveis_portao_3.sirene != HIGH || niveis_portao_3.central_alarme != LOW || !sirene_do_portao_3 ||
        !rele_8_desligado_no_portao_3) {
      print_log("teste_portoes_da_placa_e_rele_alarme: arrombamento do portao 3 nao ligou a sirene nem desligou o rele 8.");
      break;
    }

    quantidade_portoes_placa = 1;
    if (sensorBloqueiaIntertravamento(2, false) || portaoExisteNaPlaca(2) || portaoExisteNaPlaca(3)) {
      print_log("teste_portoes_da_placa_e_rele_alarme: placa de 1 portao considerou outro portao.");
      break;
    }

    quantidade_portoes_placa = 2;
    manutencao_portao_2 = false;
    if (!sensorBloqueiaIntertravamento(2, false) || sensorBloqueiaIntertravamento(3, false) ||
        sensorBloqueiaIntertravamento(2, true)) {
      print_log("teste_portoes_da_placa_e_rele_alarme: placa de 2 portoes nao limitou o intertravamento a 1 e 2.");
      break;
    }
    manutencao_portao_2 = true;
    if (sensorBloqueiaIntertravamento(2, false)) {
      print_log("teste_portoes_da_placa_e_rele_alarme: portao em manutencao continuou bloqueando o intertravamento.");
      break;
    }

    intertravamento_ativo = true;
    EstadoPortao portao_em_manutencao;
    iniciaEstadoPortao(portao_em_manutencao, 1, false, true, 0, true);
    if (!processaPortao(portao_em_manutencao, true, true, 100, true, false, true) ||
        !portao_em_manutencao.motor_acionado) {
      print_log("teste_portoes_da_placa_e_rele_alarme: manutencao nao liberou a abertura do proprio portao.");
      break;
    }

    quantidade_portoes_placa = 3;
    EstadoPortao portao_alarme;
    EstadoPortao portao_quieto;
    iniciaEstadoPortao(portao_alarme, 1, false, true, 0, true);
    iniciaEstadoPortao(portao_quieto, 2, false, true, 0, true);
    processaPortao(portao_alarme, false, false, 100);
    NiveisSaidas niveis = calculaSaidas(portao_alarme, portao_quieto, true, true, 100, false, nullptr, true, 3);
    uint8_t byte_saidas = montaByteSaidasPca9534(niveis);
    bool rele_8_aberto = (byte_saidas & (uint8_t)(1 << (canal_saida_central_alarme - 1))) == 0;
    bool sirene_ligada = (byte_saidas & (uint8_t)(1 << (canal_saida_sirene - 1))) != 0;
    if (niveis.sirene != HIGH || niveis.central_alarme != LOW || !sirene_ligada || !rele_8_aberto) {
      print_log("teste_portoes_da_placa_e_rele_alarme: rele 8 nao abriu quando a sirene disparou.");
      break;
    }
    niveis = calculaSaidas(portao_quieto, portao_quieto, true, true, 0, false, nullptr, true, 1);
    if (niveis.motor_portao_2 != LOW || niveis.fotocelula_portao_2 != HIGH || niveis.fotocelula_portao_3 != HIGH) {
      print_log("teste_portoes_da_placa_e_rele_alarme: placa de 1 portao mexeu na saida dos outros portoes.");
      break;
    }

    resultado = true;
  } while (false);

  quantidade_portoes_placa = quantidade_original;
  manutencao_portao_2 = manutencao_portao_2_original;
  intertravamento_ativo = intertravamento_original;
  return resultado;
}

// Verifica o limite de gate_broken: sem envio anterior nunca bloqueia; após um envio bloqueia por
// intervalo_minimo_entre_gate_broken_ms e libera ao completar o intervalo, inclusive no estouro do millis().
// Restaura o estado original ao final.
bool teste_limite_gate_broken() {
  bool gate_broken_ja_enviado_original = gate_broken_ja_enviado;
  unsigned long momento_ultimo_gate_broken_original = momento_ultimo_gate_broken_enviado;
  bool resultado = false;

  do {
    gate_broken_ja_enviado = false;
    momento_ultimo_gate_broken_enviado = 0;
    if (gateBrokenDentroDoIntervaloMinimo(0) || gateBrokenDentroDoIntervaloMinimo(1000)) {
      print_log("teste_limite_gate_broken: bloqueou gate_broken sem nenhum envio anterior.");
      break;
    }

    gate_broken_ja_enviado = true;
    momento_ultimo_gate_broken_enviado = 1000;
    if (!gateBrokenDentroDoIntervaloMinimo(1000) ||
        !gateBrokenDentroDoIntervaloMinimo(1000 + intervalo_minimo_entre_gate_broken_ms - 1)) {
      print_log("teste_limite_gate_broken: gate_broken liberado antes de 1 minuto do ultimo envio.");
      break;
    }
    if (gateBrokenDentroDoIntervaloMinimo(1000 + intervalo_minimo_entre_gate_broken_ms)) {
      print_log("teste_limite_gate_broken: gate_broken continuou bloqueado apos 1 minuto do ultimo envio.");
      break;
    }

    momento_ultimo_gate_broken_enviado = 0xFFFFFFFFUL - 10;
    if (!gateBrokenDentroDoIntervaloMinimo(100)) {
      print_log("teste_limite_gate_broken: bloqueio falhou no estouro do millis().");
      break;
    }

    resultado = true;
  } while (false);

  gate_broken_ja_enviado = gate_broken_ja_enviado_original;
  momento_ultimo_gate_broken_enviado = momento_ultimo_gate_broken_original;
  return resultado;
}

// Verifica o down_siren: com abertura indevida em um portão e alerta de tempo aberto no outro, desligar a sirene
// encerra todos os alarmes (SAÍDA 7 em LOW e SAÍDA 8 em HIGH), o alerta não volta logo em seguida com o portão ainda
// aberto (só depois de um novo tempo_maximo_portao_aberto_ms), não sobra sirene pós-fechamento e uma nova abertura
// sem autorização volta a disparar o alarme.
bool teste_desligar_sirene_remoto() {
  EstadoPortao portao_1;
  EstadoPortao portao_2;
  iniciaEstadoPortao(portao_1, 1, false, true, 0, true);
  iniciaEstadoPortao(portao_2, 2, false, true, 0, true);

  processaPortao(portao_1, false, false, 100);
  processaPortao(portao_2, true, true, 100);
  processaPortao(portao_2, false, false, 200);
  unsigned long inicio_alerta = 200 + tempo_maximo_portao_aberto_ms;
  processaPortao(portao_1, false, false, inicio_alerta);
  processaPortao(portao_2, false, false, inicio_alerta);
  if (!portao_1.alarme_abertura_indevida_ativo || !portao_2.alerta_tempo_aberto_ativo ||
      calculaSaidas(portao_1, portao_2, true, true, inicio_alerta).sirene != HIGH) {
    print_log("teste_desligar_sirene_remoto: cenario inicial nao ligou a sirene.");
    return false;
  }

  desligaSirenePortao(portao_1, inicio_alerta);
  desligaSirenePortao(portao_2, inicio_alerta);
  NiveisSaidas niveis = calculaSaidas(portao_1, portao_2, true, true, inicio_alerta);
  if (portao_1.alarme_abertura_indevida_ativo || portao_1.alerta_tempo_aberto_ativo ||
      portao_2.alerta_tempo_aberto_ativo || niveis.sirene != LOW || niveis.central_alarme != HIGH) {
    print_log("teste_desligar_sirene_remoto: down_siren nao desligou a sirene.");
    return false;
  }

  processaPortao(portao_1, false, false, inicio_alerta + 1000);
  processaPortao(portao_2, false, false, inicio_alerta + 1000);
  if (portao_1.alerta_tempo_aberto_ativo || portao_2.alerta_tempo_aberto_ativo ||
      calculaSaidas(portao_1, portao_2, true, true, inicio_alerta + 1000).sirene != LOW) {
    print_log("teste_desligar_sirene_remoto: sirene voltou logo apos o down_siren com o portao aberto.");
    return false;
  }

  processaPortao(portao_2, false, false, inicio_alerta + tempo_maximo_portao_aberto_ms);
  if (!portao_2.alerta_tempo_aberto_ativo) {
    print_log("teste_desligar_sirene_remoto: alerta de tempo aberto nao voltou apos um novo tempo maximo.");
    return false;
  }

  processaPortao(portao_1, false, true, inicio_alerta + 2000);
  if (portao_1.sirene_apos_fechamento_indevido_ativa) {
    print_log("teste_desligar_sirene_remoto: fechamento apos down_siren ligou a sirene pos-fechamento.");
    return false;
  }
  processaPortao(portao_1, false, false, inicio_alerta + 3000);
  if (!portao_1.alarme_abertura_indevida_ativo) {
    print_log("teste_desligar_sirene_remoto: nova abertura indevida nao disparou o alarme apos o down_siren.");
    return false;
  }
  return true;
}

// Verifica a leitura das ações no formato real devolvido por /plaque-queues-actions/list (id numérico, action_data
// "0"/"1"/"2", fotocelula_1/fotocelula_2, down_siren, created_at) e a validade das ações imediatas: open_or_close_gate
// criado há mais de validade_acao_imediata_fila_segundos é expirado; sem relógio sincronizado, sem created_at ou com
// created_at no futuro nunca é expirado.
bool teste_fila_api_formato_atual() {
  const String payload_portao =
      "{\"message\":\"A\\u00e7\\u00f5es listadas com sucesso\",\"list\":[{\"id\":228,\"client_id\":2,\"plaque_id\":14,"
      "\"plaque_model_id\":3,\"user_id\":1,\"action_type\":\"open_or_close_gate\",\"action_data\":\"2\","
      "\"action_status\":\"pending\",\"created_at\":\"2026-10-15 09:44:22\",\"updated_at\":\"2026-10-15 09:44:22\"}],"
      "\"status\":200}";
  const String payload_fotocelula =
      "{\"message\":\"ok\",\"list\":[{\"id\":229,\"client_id\":2,\"plaque_id\":14,\"plaque_model_id\":3,\"user_id\":1,"
      "\"action_type\":\"fotocelula_2\",\"action_data\":\"0\",\"action_status\":\"pending\","
      "\"created_at\":\"2026-10-15 09:45:07\",\"updated_at\":\"2026-10-15 09:45:07\"}],\"status\":200}";
  const String payload_sirene =
      "{\"message\":\"ok\",\"list\":[{\"id\":231,\"client_id\":2,\"plaque_id\":14,\"plaque_model_id\":3,\"user_id\":1,"
      "\"action_type\":\"down_siren\",\"action_data\":\"\",\"action_status\":\"pending\","
      "\"created_at\":\"2026-10-15 09:58:10\",\"updated_at\":\"2026-10-15 09:58:10\"}],\"status\":200}";

  if (extrairValorJsonNumero(payload_portao, "id") != "228" ||
      extrairValorJsonString(payload_portao, "action_type") != "open_or_close_gate" ||
      extrairValorJsonString(payload_portao, "action_status") != "pending" ||
      extraiNumeroPortaoDoActionData(payload_portao) != 2) {
    print_log("teste_fila_api_formato_atual: open_or_close_gate do formato atual lido errado.");
    return false;
  }
  if (extrairValorJsonString(payload_fotocelula, "action_type") != "fotocelula_2" ||
      extraiPortaoFotocelula("fotocelula_2", payload_fotocelula) != 2 || extraiFlagDoActionData(payload_fotocelula) != 0) {
    print_log("teste_fila_api_formato_atual: fotocelula_2 do formato atual lido errado.");
    return false;
  }
  if (extrairValorJsonString(payload_sirene, "action_type") != "down_siren" ||
      extrairValorJsonNumero(payload_sirene, "id") != "231") {
    print_log("teste_fila_api_formato_atual: down_siren do formato atual lido errado.");
    return false;
  }

  // 2026-10-15 09:44:22 em Brasília = 2026-10-15 12:44:22 UTC.
  const long long criado_em_epoch = 1792068262LL;
  if (acaoDaFilaExpirada(payload_portao, criado_em_epoch + 5) ||
      acaoDaFilaExpirada(payload_portao, criado_em_epoch + validade_acao_imediata_fila_segundos)) {
    print_log("teste_fila_api_formato_atual: acao recente foi considerada expirada.");
    return false;
  }
  if (!acaoDaFilaExpirada(payload_portao, criado_em_epoch + validade_acao_imediata_fila_segundos + 1)) {
    print_log("teste_fila_api_formato_atual: acao antiga nao foi considerada expirada.");
    return false;
  }
  if (acaoDaFilaExpirada(payload_portao, 0) || acaoDaFilaExpirada(payload_portao, criado_em_epoch - 3600) ||
      acaoDaFilaExpirada("{\"list\":[{\"id\":1,\"action_type\":\"down_siren\"}]}", criado_em_epoch + 100000)) {
    print_log("teste_fila_api_formato_atual: expirou acao sem relogio, no futuro ou sem created_at.");
    return false;
  }
  return true;
}

// Confere escrita e leitura do bit da sirene no PCA9534 e a polaridade ativa-baixa do sensor.
bool teste_comunicacao_pca9534() {
  uint8_t byte_original = 0;
  if (!leRegistradorPca9534ComSucesso(endereco_pca9534_saidas, registrador_saida_pca9534, byte_original)) {
    print_log("teste_comunicacao_pca9534: nao leu o registrador de saida do PCA9534 0x20.");
    return false;
  }

  uint8_t byte_sirene = (uint8_t)(byte_original ^ (uint8_t)(1 << (canal_saida_sirene - 1)));
  if (!escreveRegistradorPca9534(endereco_pca9534_saidas, registrador_saida_pca9534, byte_sirene)) {
    escreveRegistradorPca9534(endereco_pca9534_saidas, registrador_saida_pca9534, byte_original);
    print_log("teste_comunicacao_pca9534: nao gravou o bit da sirene.");
    return false;
  }

  uint8_t byte_lido = 0;
  bool leitura_ok = leRegistradorPca9534ComSucesso(endereco_pca9534_saidas, registrador_saida_pca9534, byte_lido);
  escreveRegistradorPca9534(endereco_pca9534_saidas, registrador_saida_pca9534, byte_original);
  if (!leitura_ok || byte_lido != byte_sirene) {
    print_log("teste_comunicacao_pca9534: readback da sirene diferente do byte gravado.");
    return false;
  }

  uint8_t polaridade = 0xFF;
  if (!leRegistradorPca9534ComSucesso(endereco_pca9534_entradas, registrador_polaridade_pca9534, polaridade) ||
      polaridade != 0x00) {
    print_log("teste_comunicacao_pca9534: polaridade do PCA9534 de entradas nao esta em 0x00.");
    return false;
  }

  if (!canalEntradaAtivo(0x00, canal_entrada_sensor_portao_1) || canalEntradaAtivo(0xFF, canal_entrada_sensor_portao_1) ||
      !canalEntradaAtivo(0x00, canal_entrada_sensor_portao_2) || canalEntradaAtivo(0xFF, canal_entrada_sensor_portao_2)) {
    print_log("teste_comunicacao_pca9534: sensor nao esta em polaridade ativa-baixa.");
    return false;
  }

  uint8_t configuracao_entradas = 0x00;
  if (!leRegistradorPca9534ComSucesso(endereco_pca9534_entradas, registrador_configuracao_pca9534, configuracao_entradas) ||
      configuracao_entradas != 0xFF) {
    print_log("teste_comunicacao_pca9534: ENTRADA 4 pode estar presa; configuracao lida 0x" +
              String(configuracao_entradas, HEX) + ".");
    return false;
  }
  return true;
}

// Confere o intervalo de 5 s e a política de auto-reconnect (persistido, pendente, rejeitado e sem SSID).
bool teste_estabilidade_wifi_esp32_s3() {
  if (INTERVALO_RECONECTAR_WIFI_MS != 5000) {
    print_log("teste_estabilidade_wifi_esp32_s3: intervalo de reconexao diferente de 5 s.");
    return false;
  }

  bool setup_original = setup_finished;
  String ssid_original = wifi_ssid;
  bool pendente_original = wifiCredenciaisPendentes;
  bool falhou_original = wifiConfigFalhou;
  bool sucesso = true;

  setup_finished = true;
  wifi_ssid = "rede_persistida";
  wifiCredenciaisPendentes = false;
  wifiConfigFalhou = false;
  if (!deveManterReconexaoAutomaticaWifi()) {
    print_log("teste_estabilidade_wifi_esp32_s3: credencial persistida nao manteve a reconexao automatica.");
    sucesso = false;
  }

  wifiCredenciaisPendentes = true;
  if (sucesso && deveManterReconexaoAutomaticaWifi()) {
    print_log("teste_estabilidade_wifi_esp32_s3: config_plaque pendente manteve a reconexao automatica.");
    sucesso = false;
  }

  wifiCredenciaisPendentes = false;
  wifiConfigFalhou = true;
  if (sucesso && deveManterReconexaoAutomaticaWifi()) {
    print_log("teste_estabilidade_wifi_esp32_s3: credencial rejeitada manteve a reconexao automatica.");
    sucesso = false;
  }

  wifiConfigFalhou = false;
  wifi_ssid = "";
  if (sucesso && deveManterReconexaoAutomaticaWifi()) {
    print_log("teste_estabilidade_wifi_esp32_s3: SSID vazio manteve a reconexao automatica.");
    sucesso = false;
  }

  setup_finished = false;
  wifi_ssid = "rede_persistida";
  if (sucesso && deveManterReconexaoAutomaticaWifi()) {
    print_log("teste_estabilidade_wifi_esp32_s3: setup nao concluido manteve a reconexao automatica.");
    sucesso = false;
  }

  setup_finished = setup_original;
  wifi_ssid = ssid_original;
  wifiCredenciaisPendentes = pendente_original;
  wifiConfigFalhou = falhou_original;

  // Sem Wi-Fi persistido e com a Ethernet já iniciada, ligar e desligar o rádio derruba o W5500.
  // A política de reconexão já foi conferida; o cabo permanece com a rota do aplicativo.
  if (sucesso && ethIniciado && !wifiEstaConfigurado() && !wifiCredenciaisPendentes) {
    return true;
  }

  if (sucesso) {
    wifi_mode_t modo_original = WiFi.getMode();
    bool ligou_radio_so_para_teste = modo_original == WIFI_OFF || modo_original == WIFI_MODE_NULL;
    if (ligou_radio_so_para_teste) {
      WiFi.mode(WIFI_STA);
    }
    configuraEstabilidadeWifiEsp32S3();
    if (WiFi.getSleep() != WIFI_PS_NONE) {
      print_log("teste_estabilidade_wifi_esp32_s3: sleep do modem continua ligado.");
      sucesso = false;
    }
    if (sucesso && WiFi.getAutoReconnect() != deveManterReconexaoAutomaticaWifi()) {
      print_log("teste_estabilidade_wifi_esp32_s3: auto-reconnect nao seguiu a politica das credenciais.");
      sucesso = false;
    }
    if (ligou_radio_so_para_teste && !wifiEstaConfigurado() && !wifiCredenciaisPendentes) {
      WiFi.mode(WIFI_OFF);
    }
  }

  return sucesso;
}

// Confere decodificação, rejeição de ruído, aprendizado só com quadro repetido, código diferente e cooldown de 1 s.
bool teste_receptor_frequencia_433() {
  bool gravado_original = codigo_rf433_esta_gravado;
  uint32_t codigo_original = codigo_rf433_gravado;
  uint8_t bits_originais = quantidade_bits_codigo_rf433_gravado;
  bool gravacao_original = gravacao_codigo_rf433_pendente;
  bool aguardando_original = aguardando_repeticao_quadro_rf433;
  bool comando_original = comando_rf433_portao_1_pendente;
  unsigned long momento_original = momento_ultimo_comando_rf433_reconhecido;
  bool cadastro_original = cadastro_controle_rf433_ativo;
  uint8_t quantidade_original = quantidade_controles_rf433_gravados;
  uint32_t codigos_originais[quantidade_maxima_controles_rf433];
  uint8_t bits_originais_lista[quantidade_maxima_controles_rf433];
  for (uint8_t indice = 0; indice < quantidade_maxima_controles_rf433; indice++) {
    codigos_originais[indice] = codigos_controles_rf433[indice];
    bits_originais_lista[indice] = bits_controles_rf433[indice];
  }

  codigo_rf433_esta_gravado = false;
  codigo_rf433_gravado = 0;
  quantidade_bits_codigo_rf433_gravado = 0;
  gravacao_codigo_rf433_pendente = false;
  aguardando_repeticao_quadro_rf433 = false;
  comando_rf433_portao_1_pendente = false;
  momento_ultimo_comando_rf433_reconhecido = 0;
  quadro_rf433_injetado_pendente = false;
  cadastro_controle_rf433_ativo = false;
  quantidade_controles_rf433_gravados = 0;

  bool sucesso = true;
  uint16_t quadro_valido[] = {900, 300, 300, 900, 900, 300, 300, 900, 300, 900, 900, 300, 300, 900, 900, 300};
  uint32_t codigo = 0;
  uint8_t quantidade_bits = 0;
  if (!decodificaTremDePulsosRf433(quadro_valido, 16, codigo, quantidade_bits) || codigo != 0xA5 || quantidade_bits != 8) {
    print_log("teste_receptor_frequencia_433: decodificacao do quadro sintetico falhou.");
    sucesso = false;
  }

  uint16_t quadro_com_sincronismo[17];
  for (int indice = 0; indice < 16; indice++) {
    quadro_com_sincronismo[indice] = quadro_valido[indice];
  }
  quadro_com_sincronismo[16] = 9000;
  uint32_t codigo_sincronismo = 0;
  uint8_t bits_sincronismo = 0;
  if (sucesso && (!decodificaTremDePulsosRf433(quadro_com_sincronismo, 17, codigo_sincronismo, bits_sincronismo) ||
                  codigo_sincronismo != 0xA5)) {
    print_log("teste_receptor_frequencia_433: sincronismo longo alterou o codigo.");
    sucesso = false;
  }

  uint16_t ruido[16];
  for (int indice = 0; indice < 16; indice++) {
    ruido[indice] = 400;
  }
  uint32_t codigo_ruido = 0;
  uint8_t bits_ruido = 0;
  if (sucesso && decodificaTremDePulsosRf433(ruido, 16, codigo_ruido, bits_ruido)) {
    print_log("teste_receptor_frequencia_433: ruido foi aceito como quadro.");
    sucesso = false;
  }

  uint16_t quadro_incompleto[] = {900, 300, 300, 900, 900, 300, 300, 900};
  if (sucesso && decodificaTremDePulsosRf433(quadro_incompleto, 8, codigo_ruido, bits_ruido)) {
    print_log("teste_receptor_frequencia_433: quadro incompleto foi aceito.");
    sucesso = false;
  }

  auto injeta_quadro = [](bool valido, uint32_t codigo_injetado, uint8_t bits_injetados, unsigned long agora) {
    quadro_rf433_injetado_pendente = true;
    quadro_rf433_injetado_valido = valido;
    codigo_rf433_injetado = codigo_injetado;
    quantidade_bits_codigo_rf433_injetado = bits_injetados;
    processaReceptorFrequencia433(agora);
  };

  if (sucesso) {
    injeta_quadro(false, 0, 0, 1000);
    injeta_quadro(true, 0xA5, 8, 1100);
    injeta_quadro(true, 0xA5, 8, 1200);
    if (codigo_rf433_esta_gravado || quantidade_controles_rf433_gravados != 0) {
      print_log("teste_receptor_frequencia_433: gravou sem o botao cadastrar controle.");
      sucesso = false;
    }
  }
  if (sucesso) {
    abreCadastroControleRf433(1300);
    injeta_quadro(true, 0xA5, 8, 1400);
    if (codigo_rf433_esta_gravado) {
      print_log("teste_receptor_frequencia_433: gravou sem quadro repetido.");
      sucesso = false;
    }
  }
  if (sucesso) {
    injeta_quadro(true, 0xA5, 8, 1500);
    if (!codigo_rf433_esta_gravado || codigo_rf433_gravado != 0xA5 || !gravacao_codigo_rf433_pendente ||
        !controleRf433JaCadastrado(0xA5, 8) || comando_rf433_portao_1_pendente || cadastro_controle_rf433_ativo) {
      print_log("teste_receptor_frequencia_433: quadro repetido nao gravou ou ja acionou o portao.");
      sucesso = false;
    }
  }
  if (sucesso) {
    injeta_quadro(true, 0x5A, 8, 1600);
    injeta_quadro(true, 0x5A, 8, 1700);
    if (!controleRf433JaCadastrado(0xA5, 8) || controleRf433JaCadastrado(0x5A, 8) || comando_rf433_portao_1_pendente) {
      print_log("teste_receptor_frequencia_433: codigo diferente sobrescreveu o gravado ou acionou o portao.");
      sucesso = false;
    }
  }
  if (sucesso) {
    abreCadastroControleRf433(1800);
    injeta_quadro(true, 0x5A, 8, 1900);
    injeta_quadro(true, 0x5A, 8, 2000);
    if (!controleRf433JaCadastrado(0xA5, 8) || !controleRf433JaCadastrado(0x5A, 8) || comando_rf433_portao_1_pendente) {
      print_log("teste_receptor_frequencia_433: segundo controle nao foi cadastrado.");
      sucesso = false;
    }
  }
  if (sucesso) {
    injeta_quadro(true, 0xA5, 8, 2100);
    injeta_quadro(true, 0xA5, 8, 2200);
    if (comando_rf433_portao_1_pendente) {
      print_log("teste_receptor_frequencia_433: comando dentro do cooldown de 1 s.");
      sucesso = false;
    }
  }
  if (sucesso) {
    unsigned long momento_liberado = 2000 + intervalo_minimo_comando_rf433_ms;
    injeta_quadro(true, 0xA5, 8, momento_liberado);
    injeta_quadro(true, 0xA5, 8, momento_liberado);
    if (!consomeComandoReceptorRf433()) {
      print_log("teste_receptor_frequencia_433: codigo reconhecido nao acionou o portao 1.");
      sucesso = false;
    } else {
      EstadoPortao portao;
      iniciaEstadoPortao(portao, 1, false, true, 0, true);
      processaPortao(portao, true, true, momento_liberado);
      if (!portao.motor_acionado || !portao.abertura_autorizada) {
        print_log("teste_receptor_frequencia_433: comando RF433 nao gerou pulso no portao 1.");
        sucesso = false;
      }
    }
  }
  if (sucesso && consomeComandoReceptorRf433()) {
    print_log("teste_receptor_frequencia_433: comando RF433 foi entregue duas vezes.");
    sucesso = false;
  }

  codigo_rf433_esta_gravado = gravado_original;
  codigo_rf433_gravado = codigo_original;
  quantidade_bits_codigo_rf433_gravado = bits_originais;
  gravacao_codigo_rf433_pendente = gravacao_original;
  aguardando_repeticao_quadro_rf433 = aguardando_original;
  comando_rf433_portao_1_pendente = comando_original;
  momento_ultimo_comando_rf433_reconhecido = momento_original;
  quadro_rf433_injetado_pendente = false;
  cadastro_controle_rf433_ativo = cadastro_original;
  quantidade_controles_rf433_gravados = quantidade_original;
  for (uint8_t indice = 0; indice < quantidade_maxima_controles_rf433; indice++) {
    codigos_controles_rf433[indice] = codigos_originais[indice];
    bits_controles_rf433[indice] = bits_originais_lista[indice];
  }
  return sucesso;
}

// Confere a assinatura do quadro RF e que ela so sai na janela de 20 s do botao de gravar controle.
bool teste_assinatura_quadro_controle_rf433() {
  uint32_t capturados_originais = quantidade_quadros_assinatura_rf433;
  uint32_t regulares_originais = quantidade_quadros_assinatura_rf433_regulares;
  uint32_t rejeitados_originais = quantidade_quadros_assinatura_rf433_rejeitados;
  unsigned long mensagem_original = momento_ultima_mensagem_assinatura_rf433;
  bool comando_original = comando_rf433_portao_1_pendente;
  momento_ultima_mensagem_assinatura_rf433 = millis();

  bool sucesso = true;
  if (classificaIntervaloPulsoRf433(350) != 'S' || classificaIntervaloPulsoRf433(650) != 'S' ||
      classificaIntervaloPulsoRf433(349) != '?' || classificaIntervaloPulsoRf433(651) != '?') {
    print_log("teste_assinatura_quadro_controle_rf433: faixa do pulso curto incorreta.");
    sucesso = false;
  }
  if (sucesso && (classificaIntervaloPulsoRf433(800) != 'L' || classificaIntervaloPulsoRf433(1150) != 'L' ||
                  classificaIntervaloPulsoRf433(799) != '?' || classificaIntervaloPulsoRf433(1151) != '?')) {
    print_log("teste_assinatura_quadro_controle_rf433: faixa do pulso longo incorreta.");
    sucesso = false;
  }

  char simbolos_curtos[58];
  for (int indice = 0; indice < 57; indice++) {
    simbolos_curtos[indice] = 'S';
  }
  simbolos_curtos[57] = '\0';
  if (sucesso && calculaAssinaturaSimbolosRf433(simbolos_curtos, 57) != 0x0259279DUL) {
    print_log("teste_assinatura_quadro_controle_rf433: assinatura FNV do quadro curto nao confere.");
    sucesso = false;
  }

  uint16_t quadro_curto[57];
  for (int indice = 0; indice < 57; indice++) {
    quadro_curto[indice] = 400;
  }
  uint32_t assinatura = 0;
  if (sucesso && (!analisaQuadroAssinaturaRf433(quadro_curto, 57, assinatura) || assinatura != 0x0259279DUL)) {
    print_log("teste_assinatura_quadro_controle_rf433: quadro de 57 pulsos curtos nao gerou a assinatura esperada.");
    sucesso = false;
  }
  if (sucesso && comando_rf433_portao_1_pendente != comando_original) {
    print_log("teste_assinatura_quadro_controle_rf433: exibir a assinatura acionou o portao.");
    sucesso = false;
  }
  if (sucesso && (quantidade_quadros_assinatura_rf433 != capturados_originais + 1 ||
                  quantidade_quadros_assinatura_rf433_regulares != regulares_originais + 1 ||
                  quantidade_quadros_assinatura_rf433_rejeitados != rejeitados_originais)) {
    print_log("teste_assinatura_quadro_controle_rf433: contadores do quadro regular incorretos.");
    sucesso = false;
  }

  uint16_t quadro_longo[57];
  for (int indice = 0; indice < 57; indice++) {
    quadro_longo[indice] = 900;
  }
  uint32_t assinatura_longa = 0;
  if (sucesso && (!analisaQuadroAssinaturaRf433(quadro_longo, 57, assinatura_longa) ||
                  assinatura_longa != 0xD4900788UL || assinatura_longa == assinatura)) {
    print_log("teste_assinatura_quadro_controle_rf433: quadro longo nao gerou assinatura propria.");
    sucesso = false;
  }

  uint16_t quadro_curto_demais[49];
  for (int indice = 0; indice < 49; indice++) {
    quadro_curto_demais[indice] = 400;
  }
  if (sucesso && analisaQuadroAssinaturaRf433(quadro_curto_demais, 49, assinatura)) {
    print_log("teste_assinatura_quadro_controle_rf433: quadro com menos de 50 intervalos foi aceito.");
    sucesso = false;
  }

  uint16_t quadro_longo_demais[71];
  for (int indice = 0; indice < 71; indice++) {
    quadro_longo_demais[indice] = 400;
  }
  if (sucesso && analisaQuadroAssinaturaRf433(quadro_longo_demais, 71, assinatura)) {
    print_log("teste_assinatura_quadro_controle_rf433: quadro com mais de 70 intervalos foi aceito.");
    sucesso = false;
  }

  uint16_t quadro_irregular[57];
  for (int indice = 0; indice < 57; indice++) {
    quadro_irregular[indice] = 400;
  }
  for (int indice = 0; indice < 6; indice++) {
    quadro_irregular[indice] = 100;
  }
  if (sucesso && analisaQuadroAssinaturaRf433(quadro_irregular, 57, assinatura)) {
    print_log("teste_assinatura_quadro_controle_rf433: quadro com menos de 90% de intervalos regulares foi aceito.");
    sucesso = false;
  }

  uint16_t quadro_limite[50];
  for (int indice = 0; indice < 50; indice++) {
    quadro_limite[indice] = 400;
  }
  uint32_t assinatura_limite = 0;
  if (sucesso && (!analisaQuadroAssinaturaRf433(quadro_limite, 50, assinatura_limite) ||
                  assinatura_limite != 0xDCFB1AC3UL)) {
    print_log("teste_assinatura_quadro_controle_rf433: quadro de 50 intervalos foi rejeitado.");
    sucesso = false;
  }

  uint16_t quadro_teto[70];
  for (int indice = 0; indice < 70; indice++) {
    quadro_teto[indice] = 400;
  }
  uint32_t assinatura_teto = 0;
  if (sucesso && (!analisaQuadroAssinaturaRf433(quadro_teto, 70, assinatura_teto) || assinatura_teto != 0xD26BA6ABUL)) {
    print_log("teste_assinatura_quadro_controle_rf433: quadro de 70 intervalos foi rejeitado.");
    sucesso = false;
  }

  if (sucesso && (identificaAssinaturaControleRf433(assinatura_botao_1_controle_rf433).indexOf("BOTAO 1") < 0 ||
                  identificaAssinaturaControleRf433(assinatura_botao_2_controle_rf433).indexOf("BOTAO 2") < 0 ||
                  identificaAssinaturaControleRf433(assinatura_botao_3_controle_rf433).indexOf("BOTAO 3") < 0 ||
                  identificaAssinaturaControleRf433(0x0259279DUL).indexOf("DESCONHECIDA") < 0)) {
    print_log("teste_assinatura_quadro_controle_rf433: identificacao dos botoes 1, 2 e 3 incorreta.");
    sucesso = false;
  }

  bool cadastro_antes_da_janela = cadastro_controle_rf433_ativo;
  bool quadro_pronto_antes_da_janela = quadro_rf433_pronto_para_decodificar;
  bool quadro_excedeu_antes_da_janela = quadro_rf433_excedeu_pulsos;
  int quantidade_pulsos_antes_da_janela = quantidade_pulsos_capturados_rf433;
  uint32_t capturados_antes_da_janela = quantidade_quadros_assinatura_rf433;
  momento_ultima_mensagem_assinatura_rf433 = millis();

  for (int indice = 0; indice < 57; indice++) {
    duracoes_pulsos_rf433[indice] = 400;
  }
  quantidade_pulsos_capturados_rf433 = 57;
  quadro_rf433_excedeu_pulsos = false;
  quadro_rf433_pronto_para_decodificar = true;
  cadastro_controle_rf433_ativo = false;
  uint32_t codigo_fora_da_janela = 0;
  uint8_t bits_fora_da_janela = 0;
  consomeQuadroCapturadoRf433(codigo_fora_da_janela, bits_fora_da_janela);
  if (sucesso && (quantidade_quadros_assinatura_rf433 != capturados_antes_da_janela ||
                  comando_rf433_portao_1_pendente != comando_original)) {
    print_log("teste_assinatura_quadro_controle_rf433: quadro RF foi lido fora da janela de 20 s ou acionou o rele.");
    sucesso = false;
  }

  for (int indice = 0; indice < 57; indice++) {
    duracoes_pulsos_rf433[indice] = 400;
  }
  quantidade_pulsos_capturados_rf433 = 57;
  quadro_rf433_excedeu_pulsos = false;
  quadro_rf433_pronto_para_decodificar = true;
  cadastro_controle_rf433_ativo = true;
  uint32_t codigo_dentro_da_janela = 0;
  uint8_t bits_dentro_da_janela = 0;
  consomeQuadroCapturadoRf433(codigo_dentro_da_janela, bits_dentro_da_janela);
  if (sucesso && (quantidade_quadros_assinatura_rf433 != capturados_antes_da_janela + 1 ||
                  comando_rf433_portao_1_pendente != comando_original)) {
    print_log("teste_assinatura_quadro_controle_rf433: quadro RF nao foi lido na janela de 20 s ou acionou o rele.");
    sucesso = false;
  }

  cadastro_controle_rf433_ativo = cadastro_antes_da_janela;
  quadro_rf433_pronto_para_decodificar = quadro_pronto_antes_da_janela;
  quadro_rf433_excedeu_pulsos = quadro_excedeu_antes_da_janela;
  quantidade_pulsos_capturados_rf433 = quantidade_pulsos_antes_da_janela;

  quantidade_quadros_assinatura_rf433 = capturados_originais;
  quantidade_quadros_assinatura_rf433_regulares = regulares_originais;
  quantidade_quadros_assinatura_rf433_rejeitados = rejeitados_originais;
  momento_ultima_mensagem_assinatura_rf433 = mensagem_original;
  comando_rf433_portao_1_pendente = comando_original;
  return sucesso;
}

// Executa todos os testes da placa. Retorna false (e mostra o motivo) no primeiro teste que falhar.
// Verifica os GPIOs dos três LEDs e o ritmo de 1 s aceso e 1 s apagado do LED de status.
bool teste_leds_indicadores_rede() {
  if (pino_led_wifi != 3 || pino_led_ethernet != 21 || pino_led_status_servidor != 47) {
    print_log("teste_leds_indicadores_rede: GPIO do LED diferente do manual (D6=3, D7=21, D8=47).");
    return false;
  }

  const uint8_t pinos_reservados[] = {8, 9, 48, 5, 11, 12, 13, 14, 10, 7, 15, 16, 19, 20};
  const uint8_t pinos_led[] = {pino_led_wifi, pino_led_ethernet, pino_led_status_servidor};
  for (uint8_t pino_led : pinos_led) {
    for (uint8_t pino_reservado : pinos_reservados) {
      if (pino_led == pino_reservado) {
        print_log("teste_leds_indicadores_rede: LED no GPIO de I2C, Ethernet, RF, USB ou RTC.");
        return false;
      }
    }
  }

  if (tempo_piscada_led_status_servidor_ms != 1000) {
    print_log("teste_leds_indicadores_rede: ritmo diferente de 1 segundo.");
    return false;
  }

  if (nivelLedConexao(true) != HIGH || nivelLedConexao(false) != LOW) {
    print_log("teste_leds_indicadores_rede: LED de Wi-Fi ou cabo nao acompanha a conexao.");
    return false;
  }

  if (nivelLedStatusServidor(0) != HIGH ||
      nivelLedStatusServidor(tempo_piscada_led_status_servidor_ms - 1) != HIGH ||
      nivelLedStatusServidor(tempo_piscada_led_status_servidor_ms) != LOW ||
      nivelLedStatusServidor((tempo_piscada_led_status_servidor_ms * 2) - 1) != LOW ||
      nivelLedStatusServidor(tempo_piscada_led_status_servidor_ms * 2) != HIGH) {
    print_log("teste_leds_indicadores_rede: LED de status fora do ritmo 1 s aceso e 1 s apagado.");
    return false;
  }

  return true;
}

// Sem Wi-Fi persistido, o cabo com IP confirma a configuração e as requisições não passam pelo rádio.
bool teste_cabo_assume_comunicacao_sem_wifi() {
  if (strcmp(statusDaConfiguracaoQuandoWifiNaoSobe(true), "completed") != 0) {
    print_log("teste_cabo_assume_comunicacao_sem_wifi: cabo disponivel nao confirmou a configuracao.");
    return false;
  }
  if (strcmp(statusDaConfiguracaoQuandoWifiNaoSobe(false), "failed") != 0) {
    print_log("teste_cabo_assume_comunicacao_sem_wifi: sem cabo a configuracao nao falhou.");
    return false;
  }
  if (!caboDeveAssumirConfiguracaoDoAplicativo(true, false, false)) {
    print_log("teste_cabo_assume_comunicacao_sem_wifi: cabo sem Wi-Fi persistido nao assumiu a configuracao.");
    return false;
  }
  if (caboDeveAssumirConfiguracaoDoAplicativo(false, false, false) ||
      caboDeveAssumirConfiguracaoDoAplicativo(true, true, false) ||
      caboDeveAssumirConfiguracaoDoAplicativo(true, false, true)) {
    print_log("teste_cabo_assume_comunicacao_sem_wifi: cabo assumiu a configuracao com Wi-Fi no ar, ja persistido ou sem IP.");
    return false;
  }

  bool setup_original = setup_finished;
  String ssid_original = wifi_ssid;
  String senha_original = wifi_password;
  bool pendente_original = wifiCredenciaisPendentes;
  bool sucesso = true;

  setup_finished = false;
  wifi_ssid = "";
  wifi_password = "";
  wifiCredenciaisPendentes = false;
  if (deveTentarWifiAntesDoCabo() != wifiConectado()) {
    print_log("teste_cabo_assume_comunicacao_sem_wifi: sem credencial a requisicao nao ficou no cabo.");
    sucesso = false;
  }

  setup_finished = true;
  wifi_ssid = "rede_persistida";
  wifi_password = "senha_persistida";
  wifiCredenciaisPendentes = false;
  if (sucesso && !deveTentarWifiAntesDoCabo()) {
    print_log("teste_cabo_assume_comunicacao_sem_wifi: Wi-Fi persistido deixou de ser tentado antes do cabo.");
    sucesso = false;
  }

  setup_finished = false;
  wifi_ssid = "";
  wifi_password = "";
  wifiCredenciaisPendentes = true;
  if (sucesso && !deveTentarWifiAntesDoCabo()) {
    print_log("teste_cabo_assume_comunicacao_sem_wifi: config_plaque pendente deixou de tentar o Wi-Fi.");
    sucesso = false;
  }

  setup_finished = setup_original;
  wifi_ssid = ssid_original;
  wifi_password = senha_original;
  wifiCredenciaisPendentes = pendente_original;

  if (sucesso && !setup_original && ssid_original.length() == 0 && !pendente_original) {
    wifi_mode_t modo_wifi = WiFi.getMode();
    bool conectado_antes = wifiConectado();
    wifi_mode_t modo_depois = WiFi.getMode();
    if (conectado_antes) {
      print_log("teste_cabo_assume_comunicacao_sem_wifi: Wi-Fi conectou sem credencial persistida.");
      sucesso = false;
    }
    if (sucesso && modo_wifi != modo_depois) {
      print_log("teste_cabo_assume_comunicacao_sem_wifi: consultar o Wi-Fi ligou o radio e pode derrubar o cabo.");
      sucesso = false;
    }
    if (sucesso && modo_wifi != WIFI_OFF && modo_wifi != WIFI_MODE_NULL) {
      print_log("teste_cabo_assume_comunicacao_sem_wifi: radio Wi-Fi ligado sem credencial.");
      sucesso = false;
    }
  }

  return sucesso;
}

// A abertura pelo aplicativo não consulta Wi-Fi nem Ethernet na tarefa dos portões: o LED usa a cópia feita no loop.
bool teste_abertura_pelo_aplicativo_nao_consulta_a_rede() {
  if (tempo_espera_resultado_comando_remoto_ms < 1000) {
    print_log("teste_abertura_pelo_aplicativo_nao_consulta_a_rede: espera do comando de abertura menor que 1 s.");
    return false;
  }

  bool wifi_original = wifi_conectado_para_led;
  bool cabo_original = cabo_conectado_para_led;
  wifi_conectado_para_led = true;
  cabo_conectado_para_led = false;
  bool wifi_aceso = nivelLedConexao(wifi_conectado_para_led) == HIGH;
  bool cabo_apagado = nivelLedConexao(cabo_conectado_para_led) == LOW;
  wifi_conectado_para_led = false;
  cabo_conectado_para_led = true;
  bool wifi_apagado = nivelLedConexao(wifi_conectado_para_led) == LOW;
  bool cabo_aceso = nivelLedConexao(cabo_conectado_para_led) == HIGH;
  wifi_conectado_para_led = wifi_original;
  cabo_conectado_para_led = cabo_original;

  if (!wifi_aceso || !cabo_apagado || !wifi_apagado || !cabo_aceso) {
    print_log("teste_abertura_pelo_aplicativo_nao_consulta_a_rede: LED nao segue a copia da rede lida no loop.");
    return false;
  }
  return true;
}

// Confere cadastro da assinatura, atualização pela fila, pulso só quando vinculado e o corpo dos envios à API.
bool teste_gerenciamento_botoes_controle() {
  BotaoControleCadastrado copia_botoes[quantidade_maxima_botoes_controle];
  uint8_t quantidade_original = quantidade_botoes_controle;
  bool gravacao_original = gravacao_botoes_controle_pendente;
  uint32_t geracao_original = geracao_botoes_controle;
  memcpy(copia_botoes, botoes_controle, sizeof(copia_botoes));

  bool gravado_original = codigo_rf433_esta_gravado;
  uint32_t codigo_original = codigo_rf433_gravado;
  uint8_t bits_originais = quantidade_bits_codigo_rf433_gravado;
  bool gravacao_rf_original = gravacao_codigo_rf433_pendente;
  bool aguardando_original = aguardando_repeticao_quadro_rf433;
  bool comando_rf_original = comando_rf433_portao_1_pendente;
  unsigned long momento_original = momento_ultimo_comando_rf433_reconhecido;
  bool cadastro_original = cadastro_controle_rf433_ativo;
  uint32_t codigo_candidato_original = codigo_rf433_candidato;
  uint8_t bits_candidato_original = quantidade_bits_codigo_rf433_candidato;
  bool assinatura_candidata_disponivel_original = assinatura_rf433_candidata_disponivel;
  uint32_t assinatura_candidata_original = assinatura_rf433_candidata;
  uint8_t comando_botao_original = comando_botao_controle_portao_pendente;

  portENTER_CRITICAL(&protecao_botoes_controle);
  memset(botoes_controle, 0, sizeof(botoes_controle));
  quantidade_botoes_controle = 0;
  gravacao_botoes_controle_pendente = false;
  portEXIT_CRITICAL(&protecao_botoes_controle);
  comando_botao_controle_portao_pendente = 0;
  comando_rf433_portao_1_pendente = false;
  aguardando_repeticao_quadro_rf433 = false;
  cadastro_controle_rf433_ativo = false;
  assinatura_rf433_injetada_pendente = false;
  quadro_rf433_injetado_pendente = false;

  bool sucesso = true;
  if (strcmp(chave_armazenamento_botoes_controle, "buttons") != 0 ||
      !api_url_aplicativo.endsWith("/api/app/v1") ||
      strcmp(rota_cadastro_botao_controle, "/plaque-buttons/save") != 0 ||
      strcmp(rota_atualizacao_botao_controle, "/plaque-buttons/update") != 0 ||
      strcmp(rota_log_pulso_botao_controle, "/plaques/logs") != 0) {
    print_log("teste_gerenciamento_botoes_controle: indice ou rota da API diferente do combinado.");
    sucesso = false;
  }

  String identificador_a = textoIdentificadorBotaoControle(assinatura_botao_1_controle_rf433);
  String identificador_b = textoIdentificadorBotaoControle(0x0259279DUL);
  if (sucesso && (!adicionaBotaoControleLivre(assinatura_botao_1_controle_rf433) || quantidade_botoes_controle != 1 ||
                  !gravacao_botoes_controle_pendente)) {
    print_log("teste_gerenciamento_botoes_controle: assinatura nova nao entrou em buttons.");
    sucesso = false;
  }
  ResultadoConsultaBotaoControle consulta = consultaBotaoControle(assinatura_botao_1_controle_rf433);
  if (sucesso && (!consulta.cadastrado || consulta.pode_pulsar || strcmp(consulta.status, "livre") != 0 ||
                  strcmp(consulta.numero_portao, "0") != 0 || strcmp(consulta.identificador, identificador_a.c_str()) != 0 ||
                  consulta.operacao_api_pendente != operacao_api_botao_cadastrar)) {
    print_log("teste_gerenciamento_botoes_controle: botao novo nao ficou livre no portao 0 com cadastro pendente.");
    sucesso = false;
  }
  if (sucesso && (!adicionaBotaoControleLivre(assinatura_botao_1_controle_rf433) || quantidade_botoes_controle != 1)) {
    print_log("teste_gerenciamento_botoes_controle: o re-cadastro duplicou o botao ou nao resetou.");
    sucesso = false;
  }
  consulta = consultaBotaoControle(assinatura_botao_1_controle_rf433);
  if (sucesso && (strcmp(consulta.status, "livre") != 0 || strcmp(consulta.numero_portao, "0") != 0 ||
                  consulta.operacao_api_pendente != operacao_api_botao_resetar)) {
    print_log("teste_gerenciamento_botoes_controle: re-cadastro nao voltou para livre no portao 0 com reset pendente.");
    sucesso = false;
  }
  if (sucesso && (!adicionaBotaoControleLivre(0x0259279DUL) || quantidade_botoes_controle != 2)) {
    print_log("teste_gerenciamento_botoes_controle: segundo botao nao foi acrescentado.");
    sucesso = false;
  }

  String json = montaJsonBotoesControle();
  if (sucesso && (!json.startsWith("[") || !json.endsWith("]") ||
                  json.indexOf("\"button_id\": \"" + identificador_a + "\"") < 0 ||
                  json.indexOf("\"button_id\": \"" + identificador_b + "\"") < 0 ||
                  json.indexOf("\"port_number\": \"0\"") < 0 || json.indexOf("\"status\": \"livre\"") < 0 ||
                  json.indexOf("\"api_pendente\": \"resetar\"") < 0 ||
                  json.indexOf("\"api_pendente\": \"cadastrar\"") < 0)) {
    print_log("teste_gerenciamento_botoes_controle: JSON de buttons nao guarda button_id, port_number, status e api_pendente.");
    sucesso = false;
  }
  if (sucesso && (interpretaJsonBotoesControle(json) != 2 || consultaBotaoControle(assinatura_botao_1_controle_rf433).pode_pulsar)) {
    print_log("teste_gerenciamento_botoes_controle: a leitura do JSON de buttons nao recuperou os dois botoes.");
    sucesso = false;
  }
  if (sucesso && (consultaBotaoControle(assinatura_botao_1_controle_rf433).operacao_api_pendente != operacao_api_botao_resetar ||
                  consultaBotaoControle(0x0259279DUL).operacao_api_pendente != operacao_api_botao_cadastrar)) {
    print_log("teste_gerenciamento_botoes_controle: a leitura do JSON nao recuperou cadastro ou reset pendente.");
    sucesso = false;
  }

  if (sucesso && !atualizaBotaoControleArmazenado(identificador_a, "2", "vinculado")) {
    print_log("teste_gerenciamento_botoes_controle: update do botao cadastrado falhou.");
    sucesso = false;
  }
  consulta = consultaBotaoControle(assinatura_botao_1_controle_rf433);
  if (sucesso && (!consulta.pode_pulsar || consulta.numero_portao_pulso != 2 || strcmp(consulta.status, "vinculado") != 0)) {
    print_log("teste_gerenciamento_botoes_controle: botao vinculado ao portao 2 nao liberou o pulso.");
    sucesso = false;
  }
  if (sucesso && (atualizaBotaoControleArmazenado("999999", "1", "vinculado") ||
                  atualizaBotaoControleArmazenado(identificador_b, "4", "vinculado") ||
                  atualizaBotaoControleArmazenado(identificador_b, "1", "ativo"))) {
    print_log("teste_gerenciamento_botoes_controle: aceitou botao inexistente, portao ou status invalido.");
    sucesso = false;
  }
  if (sucesso && !dadosAtualizacaoBotaoControleValidos(identificador_a, "0", "desativado")) {
    print_log("teste_gerenciamento_botoes_controle: portao 0 com status desativado foi recusado.");
    sucesso = false;
  }
  char identificador_hexadecimal[tamanho_identificador_botao_controle];
  snprintf(identificador_hexadecimal, sizeof(identificador_hexadecimal), "0x%lX",
           (unsigned long)assinatura_botao_1_controle_rf433);
  if (sucesso && !atualizaBotaoControleArmazenado(identificador_hexadecimal, "0", "livre")) {
    print_log("teste_gerenciamento_botoes_controle: button_id hexadecimal nao encontrou a assinatura.");
    sucesso = false;
  }
  consulta = consultaBotaoControle(assinatura_botao_1_controle_rf433);
  if (sucesso && (consulta.pode_pulsar || strcmp(consulta.numero_portao, "0") != 0 || strcmp(consulta.status, "livre") != 0)) {
    print_log("teste_gerenciamento_botoes_controle: botao livre no portao 0 ainda pode pulsar.");
    sucesso = false;
  }

  String payload_escapado =
      "{\"list\":[{\"id\":15,\"action_type\":\"update_button_control\",\"action_data\":\"{\\\"button_id\\\":" +
      identificador_a + ",\\\"port_number\\\":3,\\\"status\\\":\\\"vinculado\\\"}\",\"action_status\":\"pending\"}]}";
  String identificador_lido;
  String numero_portao_lido;
  String status_lido;
  if (sucesso && (!extraiAtualizacaoBotaoControleDoPayload(payload_escapado, identificador_lido, numero_portao_lido, status_lido) ||
                  identificador_lido != identificador_a || numero_portao_lido != "3" || status_lido != "vinculado")) {
    print_log("teste_gerenciamento_botoes_controle: action_data escapado de update_button_control nao foi lido.");
    sucesso = false;
  }
  String payload_objeto = "{\"list\":[{\"id\":9,\"action_type\":\"update_button_control\",\"action_data\":{\"button_id\": \"" +
                          identificador_b + "\", \"port_number\": \"1\", \"status\": \"desativado\"}}]}";
  if (sucesso && (!extraiAtualizacaoBotaoControleDoPayload(payload_objeto, identificador_lido, numero_portao_lido, status_lido) ||
                  identificador_lido != identificador_b || numero_portao_lido != "1" || status_lido != "desativado")) {
    print_log("teste_gerenciamento_botoes_controle: action_data em objeto nao foi lido.");
    sucesso = false;
  }

  String corpo_cadastro = montaCorpoCadastroBotaoControle(identificador_a);
  String corpo_reset = montaCorpoResetBotaoControle(identificador_a);
  String corpo_log = montaCorpoLogPulsoBotaoControle("gate_open", identificador_a.c_str(), "2");
  if (sucesso && (corpo_cadastro.indexOf("\"serial_id\":\"" + plaque_serial + "\"") < 0 ||
                  corpo_cadastro.indexOf("\"button_id\":" + identificador_a) < 0 ||
                  corpo_cadastro.indexOf("\"internal_token\":\"") < 0 ||
                  corpo_reset.indexOf("\"button_id\":" + identificador_a) < 0 ||
                  corpo_reset.indexOf("\"control_id\":0") < 0 || corpo_reset.indexOf("\"port_number\":0") < 0 ||
                  corpo_log.indexOf("event_type=gate_open") < 0 ||
                  corpo_log.indexOf("button_id=" + identificador_a) < 0 || corpo_log.indexOf("port_number=2") < 0 ||
                  montaCorpoLogPulsoBotaoControle("gate_close", identificador_b.c_str(), "1").indexOf("event_type=gate_close") < 0)) {
    print_log("teste_gerenciamento_botoes_controle: corpo do cadastro, do reset ou do log nao leva os campos da API.");
    sucesso = false;
  }

  auto injeta_assinatura = [](uint32_t assinatura, unsigned long momento) {
    quadro_rf433_injetado_pendente = true;
    quadro_rf433_injetado_valido = false;
    codigo_rf433_injetado = 0;
    quantidade_bits_codigo_rf433_injetado = 0;
    assinatura_rf433_injetada_pendente = true;
    assinatura_rf433_injetada = assinatura;
    processaReceptorFrequencia433(momento);
  };

  if (sucesso) {
    portENTER_CRITICAL(&protecao_botoes_controle);
    memset(botoes_controle, 0, sizeof(botoes_controle));
    quantidade_botoes_controle = 0;
    portEXIT_CRITICAL(&protecao_botoes_controle);
    abreCadastroControleRf433(50000);
    injeta_assinatura(assinatura_botao_2_controle_rf433, 50100);
    injeta_assinatura(assinatura_botao_2_controle_rf433, 50200);
    consulta = consultaBotaoControle(assinatura_botao_2_controle_rf433);
    if (!consulta.cadastrado || consulta.pode_pulsar || comando_botao_controle_portao_pendente != 0 ||
        comando_rf433_portao_1_pendente || cadastro_controle_rf433_ativo) {
      print_log("teste_gerenciamento_botoes_controle: repetir a assinatura no cadastro nao gravou livre ou ja pulsou.");
      sucesso = false;
    }
  }
  if (sucesso) {
    String identificador_pulso = textoIdentificadorBotaoControle(assinatura_botao_2_controle_rf433);
    if (!atualizaBotaoControleArmazenado(identificador_pulso, "3", "vinculado")) {
      print_log("teste_gerenciamento_botoes_controle: nao vinculou o botao ao portao 3.");
      sucesso = false;
    } else {
      momento_ultimo_comando_rf433_reconhecido = 0;
      injeta_assinatura(assinatura_botao_2_controle_rf433, 52000);
      injeta_assinatura(assinatura_botao_2_controle_rf433, 52100);
      char identificador_consumido[tamanho_identificador_botao_controle] = "";
      char numero_portao_consumido[tamanho_numero_portao_botao_controle] = "";
      uint8_t portao_consumido = consomeComandoBotaoControle(identificador_consumido, numero_portao_consumido);
      if (portao_consumido != 3 || String(identificador_consumido) != identificador_pulso ||
          String(numero_portao_consumido) != "3" || comando_rf433_portao_1_pendente ||
          consomeComandoBotaoControle(identificador_consumido, numero_portao_consumido) != 0) {
        print_log("teste_gerenciamento_botoes_controle: pulso vinculado nao foi para o portao 3 uma unica vez.");
        sucesso = false;
      }
    }
  }
  if (sucesso) {
    if (!atualizaBotaoControleArmazenado(textoIdentificadorBotaoControle(assinatura_botao_2_controle_rf433), "1", "desativado")) {
      print_log("teste_gerenciamento_botoes_controle: nao desativou o botao.");
      sucesso = false;
    } else {
      momento_ultimo_comando_rf433_reconhecido = 0;
      injeta_assinatura(assinatura_botao_2_controle_rf433, 54000);
      injeta_assinatura(assinatura_botao_2_controle_rf433, 54100);
      if (comando_botao_controle_portao_pendente != 0 || comando_rf433_portao_1_pendente) {
        print_log("teste_gerenciamento_botoes_controle: botao desativado acionou portao.");
        sucesso = false;
      }
    }
  }

  if (sucesso) {
    uint8_t quantidade_portoes_original_teste = quantidade_portoes_placa;
    bool intertravamento_original_teste = intertravamento_ativo;
    quantidade_portoes_placa = 3;
    intertravamento_ativo = true;

    const uint32_t assinaturas_sequencia[] = {assinatura_botao_3_controle_rf433, assinatura_botao_2_controle_rf433,
                                              assinatura_botao_1_controle_rf433};
    const char *identificadores_sequencia[] = {"1767424619", "3854021271", "4264037171"};
    const char *action_data_separados[] = {
        "{\"button_id\":1767424619,\"port_number\":1,\"status\":\"vinculado\"}",
        "{\"button_id\":3854021271,\"port_number\":2,\"status\":\"vinculado\"}",
        "{\"button_id\":4264037171,\"port_number\":3,\"status\":\"vinculado\"}"};

    portENTER_CRITICAL(&protecao_botoes_controle);
    memset(botoes_controle, 0, sizeof(botoes_controle));
    quantidade_botoes_controle = 0;
    gravacao_botoes_controle_pendente = false;
    portEXIT_CRITICAL(&protecao_botoes_controle);

    for (uint8_t indice = 0; indice < 3 && sucesso; indice++) {
      String texto_identificador = textoIdentificadorBotaoControle(assinaturas_sequencia[indice]);
      if (strcmp(texto_identificador.c_str(), identificadores_sequencia[indice]) != 0 ||
          !adicionaBotaoControleLivre(assinaturas_sequencia[indice])) {
        print_log("teste_gerenciamento_botoes_controle: o button_id " + String(identificadores_sequencia[indice]) +
                  " nao entrou na lista.");
        sucesso = false;
      }
    }

    for (uint8_t indice = 0; indice < 3 && sucesso; indice++) {
      ResultadoAtualizacaoBotaoControle resultado_separado;
      memset(&resultado_separado, 0, sizeof(resultado_separado));
      uint8_t quantidade_lida =
          aplicaAtualizacoesBotaoControleNoActionData(action_data_separados[indice], &resultado_separado, 1);
      String json_da_lista = montaJsonBotoesControle();
      if (quantidade_lida != 1 || !resultado_separado.localizado || !resultado_separado.atualizado_na_memoria ||
          interpretaJsonBotoesControle(json_da_lista) != 3) {
        print_log("teste_gerenciamento_botoes_controle: o evento do botao " + String(identificadores_sequencia[indice]) +
                  " nao permaneceu junto dos anteriores.");
        sucesso = false;
      }
    }

    if (sucesso) {
      for (uint8_t indice = 0; indice < 3; indice++) {
        ResultadoConsultaBotaoControle consulta_sequencia = consultaBotaoControle(assinaturas_sequencia[indice]);
        if (!consulta_sequencia.pode_pulsar || consulta_sequencia.numero_portao_pulso != (uint8_t)(indice + 1) ||
            strcmp(consulta_sequencia.status, "vinculado") != 0) {
          print_log("teste_gerenciamento_botoes_controle: o botao " + String(identificadores_sequencia[indice]) +
                    " nao ficou pronto para abrir o portao " + String(indice + 1) + ".");
          sucesso = false;
          break;
        }
      }
    }

    if (sucesso) {
      ResultadoAtualizacaoBotaoControle resultados_juntos[3];
      memset(resultados_juntos, 0, sizeof(resultados_juntos));
      String action_data_junto =
          "[{\"button_id\":1767424619,\"port_number\":1,\"status\":\"vinculado\"},"
          "{\"button_id\":3854021271,\"port_number\":2,\"status\":\"vinculado\"},"
          "{\"button_id\":4264037171,\"port_number\":3,\"status\":\"vinculado\"}]";
      if (aplicaAtualizacoesBotaoControleNoActionData(action_data_junto, resultados_juntos, 3) != 3 ||
          !resultados_juntos[0].atualizado_na_memoria || !resultados_juntos[1].atualizado_na_memoria ||
          !resultados_juntos[2].atualizado_na_memoria) {
        print_log("teste_gerenciamento_botoes_controle: um action_data com tres botoes nao atualizou os tres.");
        sucesso = false;
      }
    }

    if (sucesso) {
      EstadoPortao portao_comando_teste;
      iniciaEstadoPortao(portao_comando_teste, 1, false, true, 0, true);
      bool bloqueou = !processaPortao(portao_comando_teste, true, true, 3000, true, false, false) &&
                      !portao_comando_teste.motor_acionado && !portao_comando_teste.abertura_autorizada;
      iniciaEstadoPortao(portao_comando_teste, 1, false, true, 0, true);
      bool liberou = processaPortao(portao_comando_teste, true, true, 4000, false, false, false) &&
                     portao_comando_teste.motor_acionado && portao_comando_teste.abertura_autorizada;
      if (!bloqueou || !liberou) {
        print_log("teste_gerenciamento_botoes_controle: o pulso do botao nao respeitou o intertravamento.");
        sucesso = false;
      }
    }

    quantidade_portoes_placa = quantidade_portoes_original_teste;
    intertravamento_ativo = intertravamento_original_teste;
  }

  portENTER_CRITICAL(&protecao_botoes_controle);
  memcpy(botoes_controle, copia_botoes, sizeof(botoes_controle));
  quantidade_botoes_controle = quantidade_original;
  gravacao_botoes_controle_pendente = gravacao_original;
  geracao_botoes_controle = geracao_original;
  portEXIT_CRITICAL(&protecao_botoes_controle);
  codigo_rf433_esta_gravado = gravado_original;
  codigo_rf433_gravado = codigo_original;
  quantidade_bits_codigo_rf433_gravado = bits_originais;
  gravacao_codigo_rf433_pendente = gravacao_rf_original;
  aguardando_repeticao_quadro_rf433 = aguardando_original;
  comando_rf433_portao_1_pendente = comando_rf_original;
  momento_ultimo_comando_rf433_reconhecido = momento_original;
  cadastro_controle_rf433_ativo = cadastro_original;
  codigo_rf433_candidato = codigo_candidato_original;
  quantidade_bits_codigo_rf433_candidato = bits_candidato_original;
  assinatura_rf433_candidata_disponivel = assinatura_candidata_disponivel_original;
  assinatura_rf433_candidata = assinatura_candidata_original;
  comando_botao_controle_portao_pendente = comando_botao_original;
  assinatura_rf433_injetada_pendente = false;
  quadro_rf433_injetado_pendente = false;
  identificador_comando_botao_controle_pendente[0] = '\0';
  numero_portao_comando_botao_controle_pendente[0] = '\0';
  return sucesso;
}

bool teste_completo() {
  print_log("");
  print_log("======== TESTE COMPLETO ========");

  if (!teste_configuracao_pinos()) {
    print_log("FALHA: teste_configuracao_pinos - pinos repetidos, em conflito ou niveis de repouso errados.");
    return false;
  }
  if (!teste_estabilizacao_entrada()) {
    print_log("FALHA: teste_estabilizacao_entrada - filtro de repique das entradas incorreto.");
    return false;
  }
  if (!teste_comando_autorizado()) {
    print_log("FALHA: teste_comando_autorizado - comando autorizado nao abriu o portao sem alarme.");
    return false;
  }
  if (!teste_comando_com_portao_aberto()) {
    print_log("FALHA: teste_comando_com_portao_aberto - comando de fechamento abriu janela de autorizacao.");
    return false;
  }
  if (!teste_abertura_indevida()) {
    print_log("FALHA: teste_abertura_indevida - alarme de abertura nao autorizada incorreto.");
    return false;
  }
  if (!teste_portao_aberto_tempo_excessivo()) {
    print_log("FALHA: teste_portao_aberto_tempo_excessivo - alerta de tempo aberto incorreto.");
    return false;
  }
  if (!teste_aviso_portao_aberto_sem_fechar()) {
    print_log("FALHA: teste_aviso_portao_aberto_sem_fechar - aviso de portao aberto por comando sem fechar incorreto.");
    return false;
  }
  if (!teste_validade_comando_autorizado()) {
    print_log("FALHA: teste_validade_comando_autorizado - janela de autorizacao ou need_maintenance incorretos.");
    return false;
  }
  if (!teste_janela_abertura_comando_estendida()) {
    print_log("FALHA: teste_janela_abertura_comando_estendida - os 5 s a mais da abertura ou a sirene apos 13 s falharam.");
    return false;
  }
  if (!teste_independencia_portoes()) {
    print_log("FALHA: teste_independencia_portoes - portao 1 e portao 2 interferindo entre si.");
    return false;
  }
  if (!teste_alimentacao_fotocelula()) {
    print_log("FALHA: teste_alimentacao_fotocelula - controle das SAIDAS 4, 5 e 6 (fotocelulas) incorreto.");
    return false;
  }
  if (!teste_saida_fotocelula_e_central_alarme()) {
    print_log("FALHA: teste_saida_fotocelula_e_central_alarme - SAIDA 4/5/6 saiu de HIGH ou SAIDA 8 nao inverte a SAIDA 7.");
    return false;
  }
  if (!teste_intertravamento()) {
    print_log("FALHA: teste_intertravamento - abertura com o outro portao aberto nao foi bloqueada ou fechamento foi bloqueado.");
    return false;
  }
  if (!teste_intertravamento_tres_portoes()) {
    print_log("FALHA: teste_intertravamento_tres_portoes - abertura com outro portao aberto nao foi bloqueada entre os 3 portoes.");
    return false;
  }
  if (!teste_comando_remoto_portao()) {
    print_log("FALHA: teste_comando_remoto_portao - leitura do action_data ou acionamento via open_or_close_gate incorreto.");
    return false;
  }
  if (!teste_abertura_pelo_aplicativo_nao_consulta_a_rede()) {
    print_log("FALHA: teste_abertura_pelo_aplicativo_nao_consulta_a_rede - o comando de abertura ainda depende da consulta de rede na tarefa.");
    return false;
  }
  if (!teste_comando_flags_placa()) {
    print_log("FALHA: teste_comando_flags_placa - acoes intertravamento/fotocelula ou persistencia das flags incorretas.");
    return false;
  }
  if (!teste_modo_manutencao()) {
    print_log("FALHA: teste_modo_manutencao - sirene, alarmes, eventos ou persistencia do modo manutencao incorretos.");
    return false;
  }
  if (!teste_portoes_da_placa_e_rele_alarme()) {
    print_log("FALHA: teste_portoes_da_placa_e_rele_alarme - quantidade de portoes, intertravamento ou rele 8 incorretos.");
    return false;
  }
  if (!teste_limite_gate_broken()) {
    print_log("FALHA: teste_limite_gate_broken - intervalo minimo de 1 minuto entre gate_broken incorreto.");
    return false;
  }
  if (!teste_desligar_sirene_remoto()) {
    print_log("FALHA: teste_desligar_sirene_remoto - down_siren nao desligou a sirene corretamente.");
    return false;
  }
  if (!teste_fila_api_formato_atual()) {
    print_log("FALHA: teste_fila_api_formato_atual - leitura das acoes da fila ou validade das acoes incorreta.");
    return false;
  }
  if (!teste_estabilidade_wifi_esp32_s3()) {
    print_log("FALHA: teste_estabilidade_wifi_esp32_s3 - sleep do modem ou reconexao automatica incorretos.");
    return false;
  }
  if (!teste_cabo_assume_comunicacao_sem_wifi()) {
    print_log("FALHA: teste_cabo_assume_comunicacao_sem_wifi - sem Wi-Fi o cabo nao assumiu a comunicacao com o aplicativo.");
    return false;
  }
  if (!teste_receptor_frequencia_433()) {
    print_log("FALHA: teste_receptor_frequencia_433 - aprendizado ou reconhecimento do controle 433 incorreto.");
    return false;
  }
  if (!teste_assinatura_quadro_controle_rf433()) {
    print_log("FALHA: teste_assinatura_quadro_controle_rf433 - leitura da assinatura do controle no monitor serial incorreta.");
    return false;
  }
  if (!teste_gerenciamento_botoes_controle()) {
    print_log("FALHA: teste_gerenciamento_botoes_controle - cadastro, vinculo ou pulso dos botoes de controle incorreto.");
    return false;
  }
  if (!teste_comunicacao_pca9534()) {
    print_log("FALHA: teste_comunicacao_pca9534 - PCA9534 nao confirmou escrita da sirene ou polaridade das entradas.");
    return false;
  }
  if (!teste_leds_indicadores_rede()) {
    print_log("FALHA: teste_leds_indicadores_rede - LED de Wi-Fi, cabo ou status fora do ritmo pedido.");
    return false;
  }

  print_log("TESTE COMPLETO: todos os testes passaram.");
  print_log("================================");
  return true;
}

// ============================================================
// SETUP
// ============================================================

// Inicializa saídas em repouso, estado dos três portões, rede, testes e por fim a tarefa dos portões.
void setup() {
  Serial.begin(115200);
  delay(500);
  quantidade_portoes_placa = quantidade_portoes_configurada;
  configuraPinos();

  unsigned long agora = millis();
  bool receptora_portao_1_acionada = false;
  bool sensor_portao_1_fechado = true;
  bool receptora_portao_2_acionada = false;
  bool sensor_portao_2_fechado = true;
  bool receptora_portao_3_acionada = false;
  bool sensor_portao_3_fechado = true;
  leEntradasDeCampo(receptora_portao_1_acionada, sensor_portao_1_fechado, receptora_portao_2_acionada,
                    sensor_portao_2_fechado, receptora_portao_3_acionada, sensor_portao_3_fechado);
  if (!portaoExisteNaPlaca(2)) {
    receptora_portao_2_acionada = false;
    sensor_portao_2_fechado = true;
  }
  if (!portaoExisteNaPlaca(3)) {
    receptora_portao_3_acionada = false;
    sensor_portao_3_fechado = true;
  }

  iniciaEntradaEstabilizada(entrada_receptora_portao_1, receptora_portao_1_acionada, agora);
  iniciaEntradaEstabilizada(entrada_sensor_portao_1, sensor_portao_1_fechado, agora);
  iniciaEntradaEstabilizada(entrada_receptora_portao_2, receptora_portao_2_acionada, agora);
  iniciaEntradaEstabilizada(entrada_sensor_portao_2, sensor_portao_2_fechado, agora);
  iniciaEntradaEstabilizada(entrada_receptora_portao_3, receptora_portao_3_acionada, agora);
  iniciaEntradaEstabilizada(entrada_sensor_portao_3, sensor_portao_3_fechado, agora);
  iniciaEstadoPortao(estado_portao_1, 1, receptora_portao_1_acionada, sensor_portao_1_fechado, agora, false);
  iniciaEstadoPortao(estado_portao_2, 2, receptora_portao_2_acionada, sensor_portao_2_fechado, agora, false);
  iniciaEstadoPortao(estado_portao_3, 3, receptora_portao_3_acionada, sensor_portao_3_fechado, agora, false);
  carregaConfiguracaoPortoes();
  aplicaSaidas(calculaSaidas(estado_portao_1, estado_portao_2, fotocelula_portao_1_alimentada,
                             fotocelula_portao_2_alimentada, agora, false, &estado_portao_3,
                             fotocelula_portao_3_alimentada, quantidade_portoes_placa));

  fila_eventos_pendentes = xQueueCreate(tamanho_fila_eventos, sizeof(EventoPendente));
  fila_comandos_remotos_portao = xQueueCreate(1, sizeof(uint8_t));
  fila_resultados_comandos_remotos = xQueueCreate(1, sizeof(bool));
  fila_logs_pulsos_botoes_controle = xQueueCreate(tamanho_fila_logs_pulsos_botoes_controle, sizeof(LogPulsoBotaoControle));

  print_log("");
  print_log("======================================");
  print_log("PORTGUARD - MAV1022 / ESP32-S3");
  print_log("CONTROLE E MONITORAMENTO - " + String(quantidade_portoes_placa) + " PORTOES");
  print_log("======================================");
  print_log("ENTRADA 1 receptora portao 1 = PCA9534 0x21 canal 1");
  print_log("ENTRADA 2 sensor portao 1    = PCA9534 0x21 canal 2");
  print_log("ENTRADA 3 receptora portao 2 = PCA9534 0x21 canal 3");
  print_log("ENTRADA 4 sensor portao 2    = PCA9534 0x21 canal 4");
  print_log("ENTRADA 5 receptora portao 3 = PCA9534 0x21 canal 5");
  print_log("ENTRADA 6 sensor portao 3    = PCA9534 0x21 canal 6");
  print_log("SAIDA 1 motor portao 1       = PCA9534 0x20 canal 1 (LOW)");
  print_log("SAIDA 2 motor portao 2       = PCA9534 0x20 canal 2 (LOW)");
  print_log("SAIDA 3 motor portao 3       = PCA9534 0x20 canal 3 (LOW)");
  print_log("SAIDA 4 fotocelula portao 1  = PCA9534 0x20 canal 4 (HIGH)");
  print_log("SAIDA 5 fotocelula portao 2  = PCA9534 0x20 canal 5 (HIGH)");
  print_log("SAIDA 6 fotocelula portao 3  = PCA9534 0x20 canal 6 (HIGH)");
  print_log("SAIDA 7 sirene               = PCA9534 0x20 canal 7 (LOW)");
  print_log("SAIDA 8 central de alarme    = PCA9534 0x20 canal 8 (HIGH, inverso da sirene)");
  print_log("RF433                        = GPIO48");
  print_log("Botao cadastrar controle     = GPIO5 (ativo em nivel baixo)");
  print_log("LED Wi-Fi D6                 = GPIO3 (aceso com conexao)");
  print_log("LED Ethernet D7              = GPIO21 (aceso com cabo)");
  print_log("LED status D8                = GPIO47 (1 s aceso, 1 s apagado)");

  carregarConfigWifi();
  registraEventosWifiEsp32S3();
  iniciarEthernet();

  if (wifiEstaConfigurado()) {
    Serial.println();
    Serial.print("Modo de conexao: WIFI (");
    Serial.print(wifi_ssid);
    Serial.println("), cabo como fallback.");
    conectarWifi();
  } else {
    Serial.println();
    Serial.println("Modo de conexao: CABO DE REDE");
    Serial.println("WiFi ainda nao persistido na Preferences.");
  }

  print_log("");
  print_log(String("Estado inicial portao 1: ") + (estado_portao_1.portao_aberto ? "aberto (contagem de tempo aberto iniciada)." : "fechado."));
  print_log(String("Estado inicial portao 2: ") + (estado_portao_2.portao_aberto ? "aberto (contagem de tempo aberto iniciada)." : "fechado."));
  print_log(String("Estado inicial portao 3: ") + (estado_portao_3.portao_aberto ? "aberto (contagem de tempo aberto iniciada)." : "fechado."));

  if (fila_eventos_pendentes == nullptr) {
    print_log("ERRO: nao foi possivel criar a fila de eventos. Eventos nao serao enviados a API.");
  }

  if (!teste_completo()) {
    print_log("ATENCAO: teste_completo falhou. Verifique o motivo acima antes de instalar a placa.");
  }

  aplicaSaidas(calculaSaidas(estado_portao_1, estado_portao_2, fotocelula_portao_1_alimentada,
                             fotocelula_portao_2_alimentada, millis(), false, &estado_portao_3,
                             fotocelula_portao_3_alimentada, quantidade_portoes_placa));
  iniciaReceptorRf433();

  // Prioridade acima do loop (1) para que HTTP bloqueante no loop não atrase os portões.
  // Pilha de 12288 bytes: a tarefa lê I2C e o receptor RF433 junto da lógica dos portões.
  if (xTaskCreatePinnedToCore(tarefaControlePortoes, "controle_portoes", 12288, nullptr, 2,
                              &tarefa_controle_portoes, xPortGetCoreID()) != pdPASS) {
    print_log("ERRO: nao foi possivel iniciar a tarefa de controle dos portoes.");
  }
}

// ============================================================
// LOOP
// ============================================================

// Loop dedicado à rede: Wi-Fi/cabo, fila da API, heartbeat e envio dos eventos dos portões.
// Entradas e saídas ficam na tarefaControlePortoes.
void loop() {
  verificarRede();
  enviaEventosPendentes();
  if (gravacao_codigo_rf433_pendente) {
    salvaCodigoRf433();
  }
  if (gravacao_botoes_controle_pendente) {
    salvaBotoesControle();
  }
  enviaCadastrosBotoesControlePendentes();
  enviaLogsPulsosBotoesControle();
  delay(10);
}