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
  SAÍDA 8 / canal 8 / bit 7 - central de alarme, inverso da sirene (repouso HIGH)

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

  GPIO48 = receptor RF433 (sinal invertido por Q2)
  GPIO19/GPIO20 = USB nativa (não usar)
  GPIO15/GPIO16 = cristal do RTC (não usar)

  ------------------------------------------------------------
  FUNCIONAMENTO (portões 1, 2 e 3)
  ------------------------------------------------------------
  0. Entradas, regras e saídas rodam numa tarefa FreeRTOS própria
     (a cada 10 ms), independente das requisições HTTP do loop.
     Eventos vão para uma fila e são enviados à API com event_data {"port": N}.

  1. Comando na receptora do portão:
     - Motor do portão recebe pulso de 1 segundo.
     - Com o portão fechado, libera janela de 3 segundos para abertura.
     - Intertravamento (intertravamento_ativo): comando de abertura é
       ignorado enquanto qualquer outro portão estiver aberto. Fechamento nunca
       é bloqueado.

  2. Sensor abre sem autorização:
     - Sirene contínua enquanto o portão estiver aberto.
     - Envia gate_broken para API (no máximo 1 a cada 60 segundos,
       contando a partir do último gate_broken enviado, de qualquer portão).

  3. Portão aberto por mais de 1 minuto:
     - Sirene 1 segundo ligada / 2 segundos desligada.
     - Repete até fechar.

  4. Portão fecha:
     - Alarmes daquele portão são encerrados, exceto após abertura sem
       autorização: a sirene continua contínua por mais 120 segundos.

  SAÍDA 8 é sempre o inverso da sirene (SAÍDA 7). SAÍDAS 4, 5 e 6 mantêm as
  fotocélulas dos portões 1, 2 e 3 alimentadas (HIGH), exceto quando a ação
  fotocelula da API desliga a do portão indicado (LOW).

  5. Rede:
     - Com Wi-Fi conectado, as requisições usam Wi-Fi.
     - Se o Wi-Fi não conectar (credenciais erradas, fora de alcance, etc.),
       tenta o cabo. Se o cabo tiver IP, as requisições usam Ethernet.
     - Sem os dois, o Serial explica o que falhou em cada meio.

  6. API:
     - Consulta da fila (/plaque-queues-actions/list) a cada meio segundo
       com rede disponível (5 s de espera após uma falha); sem rede,
       tenta a cada 30 segundos.
     - Heartbeat a cada 30 segundos quando houver link.
     - config_plaque: aplica o Wi-Fi recebido em action_data.
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
     - set_maintenance / unset_maintenance: liga / desliga o modo manutenção
       (persistido). Em manutenção a sirene nunca toca, abertura sem
       autorização não arma alarme e nenhum evento/log (nem heartbeat) é
       enviado para a API; a fila continua sendo consultada e executada.
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
const String api_url = "https://portguard.milesmp.com.br/api/v1";
const String api_token = "bananapratadoisreaisoquilo";

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
const unsigned long silencio_fim_quadro_rf433_us = 5000;
const unsigned long intervalo_minimo_comando_rf433_ms = 1000;
const int quantidade_maxima_pulsos_rf433 = 80;
const int quantidade_minima_pulsos_quadro_rf433 = 16;
const int bits_minimos_quadro_rf433 = 8;
const int bits_maximos_quadro_rf433 = 32;

// Bits 0, 2 e 4 em alto: receptoras inativas. Bits 1, 3 e 5 em baixo: sensores ativos (portões fechados).
const uint8_t byte_entradas_repouso_seguro_pca9534 = 0x15;

// ============================================================
// TEMPOS DA LÓGICA DOS PORTÕES
// ============================================================

const unsigned long tempo_pulso_motor_ms = 1000;
const unsigned long janela_comando_autorizado_ms = 3000;
const unsigned long tempo_sirene_apos_fechamento_abertura_indevida_ms = 120000;
const unsigned long tempo_maximo_portao_aberto_ms = 60000;
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
// Quando true (set_maintenance), a sirene nunca toca e nenhum evento/log é enviado para a API.
// A fila de ações continua sendo consultada e executada normalmente.
volatile bool modo_manutencao_ativo = false;

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
bool quadro_rf433_injetado_pendente = false;
bool quadro_rf433_injetado_valido = false;
uint32_t codigo_rf433_injetado = 0;
uint8_t quantidade_bits_codigo_rf433_injetado = 0;

volatile uint16_t duracoes_pulsos_rf433[quantidade_maxima_pulsos_rf433];
volatile int quantidade_pulsos_capturados_rf433 = 0;
volatile uint32_t momento_ultima_borda_rf433_us = 0;
volatile bool quadro_rf433_pronto_para_decodificar = false;
volatile bool receptor_rf433_iniciado = false;

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
const unsigned long tempo_espera_resultado_comando_remoto_ms = 500;
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
bool executarPostHttp(const char *destino, const String &url, const String &body, int timeoutMs, String *payloadOut);
void falharConfigWifiRecebida();
void limparConfigWifi();
bool processarResetPlaque(long queueId);

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

bool wifiConectado() {
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
  if (niveis.central_alarme == HIGH) {
    byte_saidas |= (uint8_t)(1 << (canal_saida_central_alarme - 1));
  }
  return byte_saidas;
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

  if (modo_manutencao_ativo) {
    print_log("Modo manutencao: evento " + String(tipo_evento) + " do portao " + String(portao.numero_portao) +
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
  bool comando_bloqueado = comando_recebido && sensor_fechado && intertravamento_ativo && outro_portao_aberto;

  if (comando_bloqueado) {
    print_log(prefixo_log + "Comando de abertura" + origem_comando +
              " ignorado pelo intertravamento: o outro portao esta aberto.");
  } else if (comando_recebido) {
    portao.motor_acionado = true;
    portao.momento_inicio_pulso_motor = agora;

    if (sensor_fechado) {
      portao.abertura_autorizada = true;
      portao.momento_autorizacao = agora;
      print_log(prefixo_log + "Comando autorizado recebido" + origem_comando + ". Pulso no motor e abertura liberada por " +
                String(janela_comando_autorizado_ms / 1000) + " s.");
    } else {
      print_log(prefixo_log + "Comando recebido com portao aberto" + origem_comando + ". Pulso no motor para fechamento.");
    }
  }
  portao.receptora_anterior = receptora_acionada;

  if (portao.motor_acionado && agora - portao.momento_inicio_pulso_motor >= tempo_pulso_motor_ms) {
    portao.motor_acionado = false;
    print_log(prefixo_log + "Pulso do motor encerrado.");
  }

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
    portao.abertura_autorizada = false;
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

  if (portao.abertura_autorizada && agora - portao.momento_autorizacao > janela_comando_autorizado_ms) {
    portao.abertura_autorizada = false;
    portao.contador_possivel_falha_sensor++;
    print_log(prefixo_log + "Autorizacao expirou sem abertura. Possivel falha no sensor: " +
              String(portao.contador_possivel_falha_sensor) + "/" + String(limite_possivel_falha_sensor));

    if (portao.contador_possivel_falha_sensor >= limite_possivel_falha_sensor) {
      print_log(prefixo_log + "Limite atingido. Enviando need_maintenance.");
      registraEventoPortao(portao, "need_maintenance");
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
    registraEventoPortao(portao, "gate_timeout_open");
  }

  return !comando_bloqueado;
}

// Calcula o nível de cada saída a partir do estado dos três portões, sem escrever nos pinos.
// Abertura indevida deixa a sirene contínua enquanto o portão estiver aberto e por
// tempo_sirene_apos_fechamento_abertura_indevida_ms após fechar; tempo excessivo aberto faz a sirene intermitente.
// Com modo_manutencao a sirene fica sempre desligada (e a SAÍDA 8, inversa, em HIGH).
// portao_3 nulo mantém o motor do portão 3 desligado (testes que só exercitam dois portões).
NiveisSaidas calculaSaidas(const EstadoPortao &portao_1, const EstadoPortao &portao_2, bool alimentar_fotocelula_portao_1,
                           bool alimentar_fotocelula_portao_2, unsigned long agora, bool modo_manutencao = false,
                           const EstadoPortao *portao_3 = nullptr, bool alimentar_fotocelula_portao_3 = true) {
  const EstadoPortao *portoes[] = {&portao_1, &portao_2, portao_3};
  const int quantidade_portoes = portao_3 != nullptr ? 3 : 2;
  const unsigned long ciclo_sirene_alerta_ms = tempo_sirene_ligada_alerta_ms + tempo_sirene_desligada_alerta_ms;
  bool sirene_ligada = false;

  for (int indice = 0; indice < quantidade_portoes; indice++) {
    const EstadoPortao *portao = portoes[indice];
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
  niveis.motor_portao_2 = portao_2.motor_acionado ? HIGH : LOW;
  niveis.motor_portao_3 = portao_3 != nullptr && portao_3->motor_acionado ? HIGH : LOW;
  niveis.sirene = sirene_ligada ? HIGH : LOW;
  niveis.fotocelula_portao_1 = alimentar_fotocelula_portao_1 ? HIGH : LOW;
  niveis.fotocelula_portao_2 = alimentar_fotocelula_portao_2 ? HIGH : LOW;
  niveis.fotocelula_portao_3 = alimentar_fotocelula_portao_3 ? HIGH : LOW;
  niveis.central_alarme = sirene_ligada ? LOW : HIGH;
  return niveis;
}

// Único ponto que escreve nas saídas físicas. Regrava o byte inteiro a cada ciclo para recuperar glitch no PCA9534.
void aplicaSaidas(const NiveisSaidas &niveis) {
  static uint8_t ultimo_nivel_sirene = LOW;
  static bool escrita_saidas_falhou = false;

  uint8_t byte_saidas = montaByteSaidasPca9534(niveis);
  bool gravou = escreveRegistradorPca9534(endereco_pca9534_saidas, registrador_saida_pca9534, byte_saidas);
  if (!gravou && !escrita_saidas_falhou) {
    print_log("ERRO: falha ao gravar as saidas no PCA9534 0x20.");
  }
  escrita_saidas_falhou = !gravou;

  if (niveis.sirene != ultimo_nivel_sirene) {
    ultimo_nivel_sirene = niveis.sirene;
    print_log(niveis.sirene == HIGH ? "Sirene ligada (SAIDA 8 em LOW)." : "Sirene desligada (SAIDA 8 em HIGH).");
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

// Liga ou desliga o modo manutenção (sem sirene e sem envio de eventos/logs para a API).
void defineModoManutencao(bool ativo) {
  modo_manutencao_ativo = ativo;
  print_log(ativo ? "Modo manutencao ATIVADO: sirene bloqueada e nenhum evento/log sera enviado para a API."
                  : "Modo manutencao DESATIVADO: sirene e envio de eventos/logs voltam ao normal.");
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

// Copia o quadro fechado pela interrupção (ou pelo silêncio) e tenta decodificá-lo.
bool consomeQuadroCapturadoRf433(uint32_t &codigo, uint8_t &quantidade_bits) {
  uint16_t copia_duracoes[quantidade_maxima_pulsos_rf433];
  int quantidade = 0;

  noInterrupts();
  uint32_t desde_ultima_borda_us = micros() - momento_ultima_borda_rf433_us;
  if (!quadro_rf433_pronto_para_decodificar && receptor_rf433_iniciado &&
      quantidade_pulsos_capturados_rf433 >= quantidade_minima_pulsos_quadro_rf433 &&
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
  }
  interrupts();

  if (quantidade <= 0) {
    return false;
  }
  return decodificaTremDePulsosRf433(copia_duracoes, quantidade, codigo, quantidade_bits);
}

// Fecha a captura por silêncio, exige o mesmo quadro duas vezes, grava o primeiro código ou reconhece o já gravado.
void processaReceptorFrequencia433(unsigned long agora) {
  bool tem_quadro = false;
  bool quadro_valido = false;
  uint32_t codigo = 0;
  uint8_t quantidade_bits = 0;

  if (quadro_rf433_injetado_pendente) {
    quadro_rf433_injetado_pendente = false;
    tem_quadro = true;
    quadro_valido = quadro_rf433_injetado_valido;
    codigo = codigo_rf433_injetado;
    quantidade_bits = quantidade_bits_codigo_rf433_injetado;
  } else if (consomeQuadroCapturadoRf433(codigo, quantidade_bits)) {
    tem_quadro = true;
    quadro_valido = true;
  }

  if (!tem_quadro) {
    return;
  }
  if (!quadro_valido) {
    aguardando_repeticao_quadro_rf433 = false;
    return;
  }

  if (!aguardando_repeticao_quadro_rf433 || codigo != codigo_rf433_candidato ||
      quantidade_bits != quantidade_bits_codigo_rf433_candidato) {
    codigo_rf433_candidato = codigo;
    quantidade_bits_codigo_rf433_candidato = quantidade_bits;
    aguardando_repeticao_quadro_rf433 = true;
    return;
  }
  aguardando_repeticao_quadro_rf433 = false;

  if (!codigo_rf433_esta_gravado) {
    codigo_rf433_esta_gravado = true;
    codigo_rf433_gravado = codigo;
    quantidade_bits_codigo_rf433_gravado = quantidade_bits;
    gravacao_codigo_rf433_pendente = true;
    momento_ultimo_comando_rf433_reconhecido = agora;
    print_log("Receptor RF433: primeiro codigo repetido gravado. Os proximos acionamentos iguais abrem o portao 1.");
    return;
  }

  if (codigo != codigo_rf433_gravado || quantidade_bits != quantidade_bits_codigo_rf433_gravado) {
    print_log("Receptor RF433: codigo diferente do gravado, ignorado.");
    return;
  }

  if (agora - momento_ultimo_comando_rf433_reconhecido < intervalo_minimo_comando_rf433_ms) {
    return;
  }

  momento_ultimo_comando_rf433_reconhecido = agora;
  comando_rf433_portao_1_pendente = true;
  print_log("Receptor RF433: codigo reconhecido. Comando no portao 1.");
}

// Entrega o comando RF433 já reconhecido uma única vez para a tarefa dos portões.
bool consomeComandoReceptorRf433() {
  if (!comando_rf433_portao_1_pendente) {
    return false;
  }
  comando_rf433_portao_1_pendente = false;
  return true;
}

// Captura as durações entre bordas do GPIO48. O silêncio fecha o quadro; a decodificação fica fora da interrupção.
void IRAM_ATTR trataInterrupcaoReceptorRf433() {
  uint32_t agora_us = micros();
  uint32_t duracao_us = agora_us - momento_ultima_borda_rf433_us;
  momento_ultima_borda_rf433_us = agora_us;

  if (duracao_us >= silencio_fim_quadro_rf433_us) {
    if (quantidade_pulsos_capturados_rf433 >= quantidade_minima_pulsos_quadro_rf433) {
      quadro_rf433_pronto_para_decodificar = true;
    } else {
      quantidade_pulsos_capturados_rf433 = 0;
    }
    return;
  }

  if (quadro_rf433_pronto_para_decodificar) {
    return;
  }
  if (quantidade_pulsos_capturados_rf433 < quantidade_maxima_pulsos_rf433 && duracao_us <= 65535) {
    duracoes_pulsos_rf433[quantidade_pulsos_capturados_rf433++] = (uint16_t)duracao_us;
  }
}

// Passa a escutar o receptor 433 MHz depois dos testes, para o GPIO48 não interferir no teste_completo.
void iniciaReceptorRf433() {
  quantidade_pulsos_capturados_rf433 = 0;
  quadro_rf433_pronto_para_decodificar = false;
  momento_ultima_borda_rf433_us = micros();
  receptor_rf433_iniciado = true;
  attachInterrupt(digitalPinToInterrupt(pino_receptor_rf433), trataInterrupcaoReceptorRf433, CHANGE);
  print_log("Receptor RF433 escutando no GPIO48.");
}

// Carrega o código RF433 aprendido (rf433_ok, rf433_code, rf433_bits) da Preferences.
void carregaCodigoRf433() {
  preferences.begin("portguard", true);
  codigo_rf433_esta_gravado = preferences.getBool("rf433_ok", false);
  codigo_rf433_gravado = preferences.getUInt("rf433_code", 0);
  quantidade_bits_codigo_rf433_gravado = preferences.getUChar("rf433_bits", 0);
  preferences.end();

  if (!codigo_rf433_esta_gravado || quantidade_bits_codigo_rf433_gravado < bits_minimos_quadro_rf433) {
    codigo_rf433_esta_gravado = false;
    quantidade_bits_codigo_rf433_gravado = 0;
    print_log("Receptor RF433: nenhum codigo gravado. O proximo controle repetido sera aprendido.");
    return;
  }
  print_log("Receptor RF433: codigo gravado carregado da Preferences.");
}

// Grava o código RF433 aprendido. A tarefa só marca a pendência; quem escreve a Preferences é o loop.
void salvaCodigoRf433() {
  preferences.begin("portguard", false);
  preferences.putBool("rf433_ok", codigo_rf433_esta_gravado);
  preferences.putUInt("rf433_code", codigo_rf433_gravado);
  preferences.putUChar("rf433_bits", quantidade_bits_codigo_rf433_gravado);
  preferences.end();
  gravacao_codigo_rf433_pendente = false;
  print_log("Receptor RF433: codigo persistido na Preferences.");
}

// Carrega da Preferences as flags de intertravamento, fotocélulas dos portões 1, 2 e 3 (padrão: ligadas)
// e modo manutenção (padrão: desligado). A chave "fotocelula" é a do portão 1 (mesma de quando havia uma só).
void carregaConfiguracaoPortoes() {
  preferences.begin("portguard", true);
  intertravamento_ativo = preferences.getBool("intertravamento", true);
  fotocelula_portao_1_alimentada = preferences.getBool("fotocelula", true);
  fotocelula_portao_2_alimentada = preferences.getBool("fotocelula_2", true);
  fotocelula_portao_3_alimentada = preferences.getBool("fotocelula_3", true);
  modo_manutencao_ativo = preferences.getBool("manutencao", false);
  preferences.end();

  print_log(String("Intertravamento persistido: ") + (intertravamento_ativo ? "ativo" : "desativado"));
  print_log(String("Fotocelula portao 1 persistida: ") +
            (fotocelula_portao_1_alimentada ? "alimentada (SAIDA 4 em HIGH)" : "desligada (SAIDA 4 em LOW)"));
  print_log(String("Fotocelula portao 2 persistida: ") +
            (fotocelula_portao_2_alimentada ? "alimentada (SAIDA 5 em HIGH)" : "desligada (SAIDA 5 em LOW)"));
  print_log(String("Fotocelula portao 3 persistida: ") +
            (fotocelula_portao_3_alimentada ? "alimentada (SAIDA 6 em HIGH)" : "desligada (SAIDA 6 em LOW)"));
  print_log(String("Modo manutencao persistido: ") + (modo_manutencao_ativo ? "ativo" : "desativado"));
  carregaCodigoRf433();
}

// Salva na Preferences as flags de intertravamento, fotocélulas e modo manutenção para valerem após reiniciar a placa.
void salvaConfiguracaoPortoes() {
  preferences.begin("portguard", false);
  preferences.putBool("intertravamento", intertravamento_ativo);
  preferences.putBool("fotocelula", fotocelula_portao_1_alimentada);
  preferences.putBool("fotocelula_2", fotocelula_portao_2_alimentada);
  preferences.putBool("fotocelula_3", fotocelula_portao_3_alimentada);
  preferences.putBool("manutencao", modo_manutencao_ativo);
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

    uint8_t comando_remoto_recebido = 0;
    if (fila_comandos_remotos_portao != nullptr) {
      xQueueReceive(fila_comandos_remotos_portao, &comando_remoto_recebido, 0);
    }

    bool modo_manutencao = modo_manutencao_ativo;
    bool portao_1_aberto = !sensor_portao_1_fechado;
    bool portao_2_aberto = !sensor_portao_2_fechado;
    bool portao_3_aberto = !sensor_portao_3_fechado;
    bool comando_portao_1_executado = processaPortao(estado_portao_1, receptora_portao_1_acionada || comando_rf433,
                                                     sensor_portao_1_fechado, agora, portao_2_aberto || portao_3_aberto,
                                                     comando_remoto_recebido == 1, modo_manutencao);
    bool comando_portao_2_executado = processaPortao(estado_portao_2, receptora_portao_2_acionada, sensor_portao_2_fechado,
                                                     agora, portao_1_aberto || portao_3_aberto, comando_remoto_recebido == 2,
                                                     modo_manutencao);
    bool comando_portao_3_executado = processaPortao(estado_portao_3, receptora_portao_3_acionada, sensor_portao_3_fechado,
                                                     agora, portao_1_aberto || portao_2_aberto, comando_remoto_recebido == 3,
                                                     modo_manutencao);

    if (comando_remoto_recebido == comando_remoto_desligar_sirene) {
      desligaSirenePortao(estado_portao_1, agora);
      desligaSirenePortao(estado_portao_2, agora);
      desligaSirenePortao(estado_portao_3, agora);
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

    aplicaSaidas(calculaSaidas(estado_portao_1, estado_portao_2, fotocelula_portao_1_alimentada,
                               fotocelula_portao_2_alimentada, agora, modo_manutencao, &estado_portao_3,
                               fotocelula_portao_3_alimentada));

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

  WiFi.disconnect(true, false);
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

bool postHttpNoMeio(bool usarWifi, const char *destino, const String &url, const String &body, int timeoutMs, String *payloadOut) {
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
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

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

bool executarPostHttp(const char *destino, const String &url, const String &body, int timeoutMs, String *payloadOut) {
  bool tentarWifi = wifiConectado() || wifiEstaConfigurado() || wifiCredenciaisPendentes;

  if (tentarWifi) {
    if (postHttpNoMeio(true, destino, url, body, timeoutMs, payloadOut)) {
      return true;
    }
  }

  preferirCaboSeWifiCaiu();

  if (postHttpNoMeio(false, destino, url, body, timeoutMs, payloadOut)) {
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

bool enviarEventoLog(const char *eventType, uint8_t numero_portao) {
  // Em modo manutenção nenhum evento/log (inclusive heartbeat) vai para a API; o evento é descartado.
  if (modo_manutencao_ativo) {
    print_log("Modo manutencao: evento " + String(eventType) + " nao enviado para a API.");
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

  // event_data = {"port":N} já codificado para x-www-form-urlencoded.
  if (numero_portao > 0) {
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

// Devolve o portão de uma ação de fotocélula: fotocelula_1/2/3 = portão 1/2/3 (formato atual da
// API). Na ação fotocelula (sem número) lê a chave "port" do action_data ({"port":2,"value":0}): sem "port" = portão 1;
// 1, 2 ou 3 = esse portão; qualquer outro valor = 0 (inválido).
uint8_t extraiPortaoFotocelula(const String &tipo_acao, const String &payload) {
  if (tipo_acao == "fotocelula_1") {
    return 1;
  }
  if (tipo_acao == "fotocelula_2") {
    return 2;
  }
  if (tipo_acao == "fotocelula_3") {
    return 3;
  }

  String valor_action_data = extraiValorActionData(payload);
  int indice_chave_port = valor_action_data.indexOf("port");
  if (indice_chave_port < 0) {
    return 1;
  }

  String numero_portao = extraiPrimeiroNumeroDoTexto(valor_action_data.substring(indice_chave_port + strlen("port")));
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
// - set_maintenance / unset_maintenance: ativam / desativam o modo manutenção (action_data ignorado).
bool processarAlteracaoFlagPortoes(long queue_id, const String &tipo_acao, const String &payload) {
  bool acao_manutencao = tipo_acao == "set_maintenance" || tipo_acao == "unset_maintenance";
  bool acao_fotocelula = tipo_acao.startsWith("fotocelula");
  int valor_flag = acao_manutencao ? (tipo_acao == "set_maintenance" ? 1 : 0) : extraiFlagDoActionData(payload);
  uint8_t numero_portao_fotocelula = acao_fotocelula ? extraiPortaoFotocelula(tipo_acao, payload) : 0;
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

  if (valor_flag < 0 || (acao_fotocelula && numero_portao_fotocelula == 0)) {
    print_log(tipo_acao + " com action_data invalido (use 0 ou 1; fotocelula aceita port 1, 2 ou 3).");
    atualizarStatusFila(queue_id, "failed");
    return false;
  }

  if (acao_manutencao) {
    defineModoManutencao(valor_flag == 1);
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

  if (numero_portao == 0) {
    print_log("open_or_close_gate com action_data invalido (use vazio, 1, 2 ou 3).");
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
                              actionType == "set_maintenance" || actionType == "unset_maintenance";
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

  if (actionType == "intertravamento" || actionType.startsWith("fotocelula") || actionType == "set_maintenance" ||
      actionType == "unset_maintenance") {
    return processarAlteracaoFlagPortoes(queueId, actionType, payload);
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
    if (queueId > 0) {
      ultimoQueueIdNotificado = queueId;
      atualizarStatusFila(queueId, "failed");
    }
    return false;
  }

  if (ssid == wifiSsidRejeitado && password == wifiPasswordRejeitado) {
    Serial.println("Credenciais WiFi ja rejeitadas. Nao vou tentar de novo.");
    if (queueId > 0 && queueId != ultimoQueueIdNotificado) {
      atualizarStatusFila(queueId, "failed");
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
  queueIdPendente = queueId;
  momentoInicioConfigWifi = millis();
  ultimaTentativaWifi = 0;

  if (WiFi.status() == WL_CONNECTED) {
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

  if (queueId > 0) {
    atualizarStatusFila(queueId, "failed");
    ultimoQueueIdNotificado = queueId;
  }
  queueIdPendente = 0;

  WiFi.disconnect(true, false);
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
    WiFi.disconnect(true, false);
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

  if (!wifiConectado() && !caboConectado()) {
    reportarErroRede("nao ha conexao com a internet.");
  }

  bool redeOk = wifiConectado() || caboConectado();
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
      pino_i2c_sda, pino_i2c_scl, pino_receptor_rf433,
      ETH_SPI_MOSI, ETH_SPI_MISO, ETH_SPI_SCK, ETH_PHY_CS, ETH_PHY_IRQ, ETH_PHY_RST};
  const uint8_t pinos_esperados[] = {8, 9, 48, 11, 12, 13, 14, 10, 7};
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
         portao.eventos_registrados_teste == "need_maintenance;";
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
    NiveisSaidas niveis = calculaSaidas(portao_1, portao_2, true, true, 900, false, &portao_3, true);
    if (!portao_3.alarme_abertura_indevida_ativo || portao_3.eventos_registrados_teste.indexOf("gate_broken") < 0 ||
        niveis.sirene != HIGH || niveis.central_alarme != LOW) {
      print_log("teste_intertravamento_tres_portoes: arrombamento do portao 3 nao gerou gate_broken nem sirene.");
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
  bool modo_manutencao_original = modo_manutencao_ativo;
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

    defineModoManutencao(true);
    if (fila_eventos_pendentes != nullptr) {
      UBaseType_t eventos_antes = uxQueueMessagesWaiting(fila_eventos_pendentes);
      EstadoPortao portao_real;
      iniciaEstadoPortao(portao_real, 1, false, true, 0, false);
      registraEventoPortao(portao_real, "gate_open");
      if (uxQueueMessagesWaiting(fila_eventos_pendentes) != eventos_antes) {
        xQueueReset(fila_eventos_pendentes);
        print_log("teste_modo_manutencao: evento entrou na fila da API durante a manutencao.");
        break;
      }
    }
    if (!enviarEventoLog("heartbeat", 0)) {
      print_log("teste_modo_manutencao: enviarEventoLog nao descartou o evento durante a manutencao.");
      break;
    }

    salvaConfiguracaoPortoes();
    modo_manutencao_ativo = false;
    carregaConfiguracaoPortoes();
    if (!modo_manutencao_ativo) {
      print_log("teste_modo_manutencao: modo manutencao nao foi persistido na Preferences.");
      break;
    }

    resultado = true;
  } while (false);

  modo_manutencao_ativo = modo_manutencao_original;
  salvaConfiguracaoPortoes();
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

  codigo_rf433_esta_gravado = false;
  codigo_rf433_gravado = 0;
  quantidade_bits_codigo_rf433_gravado = 0;
  gravacao_codigo_rf433_pendente = false;
  aguardando_repeticao_quadro_rf433 = false;
  comando_rf433_portao_1_pendente = false;
  momento_ultimo_comando_rf433_reconhecido = 0;
  quadro_rf433_injetado_pendente = false;

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
    if (codigo_rf433_esta_gravado) {
      print_log("teste_receptor_frequencia_433: gravou sem quadro repetido.");
      sucesso = false;
    }
  }
  if (sucesso) {
    injeta_quadro(true, 0xA5, 8, 1200);
    if (!codigo_rf433_esta_gravado || codigo_rf433_gravado != 0xA5 || !gravacao_codigo_rf433_pendente ||
        comando_rf433_portao_1_pendente) {
      print_log("teste_receptor_frequencia_433: quadro repetido nao gravou ou ja acionou o portao.");
      sucesso = false;
    }
  }
  if (sucesso) {
    injeta_quadro(true, 0x5A, 8, 1300);
    injeta_quadro(true, 0x5A, 8, 1400);
    if (codigo_rf433_gravado != 0xA5 || comando_rf433_portao_1_pendente) {
      print_log("teste_receptor_frequencia_433: codigo diferente sobrescreveu o gravado ou acionou o portao.");
      sucesso = false;
    }
  }
  if (sucesso) {
    injeta_quadro(true, 0xA5, 8, 1500);
    injeta_quadro(true, 0xA5, 8, 1600);
    if (comando_rf433_portao_1_pendente) {
      print_log("teste_receptor_frequencia_433: comando dentro do cooldown de 1 s.");
      sucesso = false;
    }
  }
  if (sucesso) {
    unsigned long momento_liberado = 1200 + intervalo_minimo_comando_rf433_ms;
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
  return sucesso;
}

// Executa todos os testes da placa. Retorna false (e mostra o motivo) no primeiro teste que falhar.
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
  if (!teste_validade_comando_autorizado()) {
    print_log("FALHA: teste_validade_comando_autorizado - janela de autorizacao ou need_maintenance incorretos.");
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
  if (!teste_comando_flags_placa()) {
    print_log("FALHA: teste_comando_flags_placa - acoes intertravamento/fotocelula ou persistencia das flags incorretas.");
    return false;
  }
  if (!teste_modo_manutencao()) {
    print_log("FALHA: teste_modo_manutencao - sirene, alarmes, eventos ou persistencia do modo manutencao incorretos.");
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
  if (!teste_receptor_frequencia_433()) {
    print_log("FALHA: teste_receptor_frequencia_433 - aprendizado ou reconhecimento do controle 433 incorreto.");
    return false;
  }
  if (!teste_comunicacao_pca9534()) {
    print_log("FALHA: teste_comunicacao_pca9534 - PCA9534 nao confirmou escrita da sirene ou polaridade das entradas.");
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
                             fotocelula_portao_2_alimentada, agora, modo_manutencao_ativo, &estado_portao_3,
                             fotocelula_portao_3_alimentada));

  fila_eventos_pendentes = xQueueCreate(tamanho_fila_eventos, sizeof(EventoPendente));
  fila_comandos_remotos_portao = xQueueCreate(1, sizeof(uint8_t));
  fila_resultados_comandos_remotos = xQueueCreate(1, sizeof(bool));

  print_log("");
  print_log("======================================");
  print_log("PORTGUARD - MAV1022 / ESP32-S3");
  print_log("CONTROLE E MONITORAMENTO - 3 PORTOES");
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
                             fotocelula_portao_2_alimentada, millis(), modo_manutencao_ativo, &estado_portao_3,
                             fotocelula_portao_3_alimentada));
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
  delay(10);
}