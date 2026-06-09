#include <Arduino.h>
#include <Wire.h>
#include <PCF8574.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <string.h>

#define CHIP_UM 0x38
#define CHIP_DOIS 0x39

#define PUSH_PIN 4
#define SDA_PIN 21
#define SCL_PIN 22

#define MQTT_ID "esp1_pub"
#define topico_sinaleiras "nickfer/sinaleiras"
#define topic_amarelo "nickfer/comando/amarelo"
#define topic_pedestre "nickfer/comando/pedestre"

SemaphoreHandle_t mutexModoAmr;
SemaphoreHandle_t mutexPedestre;

const char *ssid = "iPhone (7)";
const char *password = "12345678";
const char *mqtt_server = "broker.emqx.io";

PCF8574 chip1(0x38);
PCF8574 chip2(0x39);

uint8_t estadoChip1 = 0xFF;
uint8_t estadoChip2 = 0xFF;

unsigned long tempoEstado = 0;
int estagioAtual = 0;
bool modoAmareloPiscante = false;
bool botaoPedestrePressionado = false;

WiFiClient espClient;
PubSubClient client(espClient);

SemaphoreHandle_t mutexEstadosChips;

typedef struct
{
  uint8_t chip;
  uint8_t comando;
} EstadoSemaforo;

typedef struct
{
  int id;
  EstadoSemaforo verde;
  EstadoSemaforo amarelo;
  EstadoSemaforo vermelho;
} Semaforo;

const Semaforo semaforos[] = {
    {1, {CHIP_DOIS, 0b11111101}, {CHIP_DOIS, 0b11111110}, {CHIP_UM, 0b01111111}},
    {2, {CHIP_UM, 0b10111111}, {CHIP_UM, 0b11011111}, {CHIP_UM, 0b11101111}},
    {3, {CHIP_UM, 0b11110111}, {CHIP_UM, 0b11111011}, {CHIP_UM, 0b11111101}},
    {4, {CHIP_DOIS, 0b11101111}, {CHIP_DOIS, 0b11110111}, {CHIP_DOIS, 0b11111011}},
    {5, {CHIP_DOIS, 0b01111111}, {CHIP_DOIS, 0b10111111}, {CHIP_DOIS, 0b11011111}}};

typedef enum
{
  COR_VERDE,
  COR_AMARELO,
  COR_VERMELHO
} CorSemaforo;

void atualizarSupervisorio(const char *mensagem)
{
  Serial.println(mensagem);
}

void escreveByte(uint8_t endereco, uint8_t valor)
{
  Wire.beginTransmission(endereco);
  Wire.write(valor);
  Wire.endTransmission();
}

void atualizarCruzamento()
{
  escreveByte(CHIP_UM, estadoChip1);
  escreveByte(CHIP_DOIS, estadoChip2);
}

void desligarTodosLeds()
{
  if (xSemaphoreTake(mutexEstadosChips, pdMS_TO_TICKS(10)) == pdTRUE)
  {
    estadoChip1 = 0xFF;
    estadoChip2 = 0xFF;
    xSemaphoreGive(mutexEstadosChips);
  }
}

void prepararLed(EstadoSemaforo estado)
{
  if (xSemaphoreTake(mutexEstadosChips, pdMS_TO_TICKS(10)) == pdTRUE)
  {
    if (estado.chip == CHIP_UM)
    {
      estadoChip1 &= estado.comando;
    }
    else if (estado.chip == CHIP_DOIS)
    {
      estadoChip2 &= estado.comando;
    }
    xSemaphoreGive(mutexEstadosChips);
  }
}

void setSemaforo(int indice, CorSemaforo cor)
{
  if (cor == COR_VERDE)
    prepararLed(semaforos[indice].verde);
  else if (cor == COR_AMARELO)
    prepararLed(semaforos[indice].amarelo);
  else if (cor == COR_VERMELHO)
    prepararLed(semaforos[indice].vermelho);
}

void handlePedestre()
{
  atualizarSupervisorio("Pedestre Solicitado - Iniciando Transição");
  uint8_t tempChip1 = estadoChip1;
  uint8_t tempChip2 = estadoChip2;

  desligarTodosLeds();
  for (int i = 0; i < 5; i++)
  {
    EstadoSemaforo comparingState = semaforos[i].verde;
    uint8_t comparisonOrig = (comparingState.chip == CHIP_UM) ? tempChip1 : tempChip2;

    if ((uint8_t)~(comparingState.comando | comparisonOrig) > 0)
    {
      setSemaforo(i, COR_AMARELO);
    }
    else
    {
      setSemaforo(i, COR_VERMELHO);
    }
  }
  atualizarCruzamento();
  delay(2000); // Tempo do amarelo antes de fechar tudo

  desligarTodosLeds();
  for (int i = 0; i < 5; i++)
  {
    setSemaforo(i, COR_VERMELHO);
  }
  atualizarCruzamento();

  atualizarSupervisorio("Pedestre atravessando...");
  delay(5000); // Tempo seguro para travessia do pedestre

  // Evita travamento infinito se segurarem o botão
  while (digitalRead(PUSH_PIN) == LOW)
  {
    delay(50);
  }

  // Limpa as flags
  xSemaphoreTake(mutexPedestre, pdMS_TO_TICKS(10));
  botaoPedestrePressionado = false;
  xSemaphoreGive(mutexPedestre);

  // Reinicia a máquina de estados principal do zero de forma segura
  estagioAtual = 0;
  tempoEstado = millis();
}

String obterCorSemaforo(int indice, uint8_t c1, uint8_t c2)
{
  // Se o modo amarelo piscante estiver ativo, não precisamos checar os bits dos estágios normais
  bool amrAtivo;
  xSemaphoreTake(mutexModoAmr, pdMS_TO_TICKS(5));
  amrAtivo = modoAmareloPiscante;
  xSemaphoreGive(mutexModoAmr);

  if (amrAtivo)
  {
    return "amarelo";
  }

  // 1. Verifica se está Verde
  EstadoSemaforo v = semaforos[indice].verde;
  uint8_t capVerde = (v.chip == CHIP_UM) ? c1 : c2;
  // Se o bit correspondente no comando for 0, e no chip também for 0, o LED está aceso
  if ((~capVerde & ~v.comando) == ~v.comando)
  {
    return "verde";
  }

  // 2. Verifica se está Amarelo
  EstadoSemaforo a = semaforos[indice].amarelo;
  uint8_t capAmarelo = (a.chip == CHIP_UM) ? c1 : c2;
  if ((~capAmarelo & ~a.comando) == ~a.comando)
  {
    return "amarelo";
  }

  // 3. Padrão: Vermelho (ou se tudo falhar)
  return "vermelho";
}

void taskPublicarSemaforos(void *pvParameters)
{
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(1000);

  for (;;)
  {
    // Aguarda o tempo de frequência definido de forma precisa
    vTaskDelayUntil(&xLastWakeTime, xFrequency);

    // Verifica se o cliente MQTT está conectado antes de tentar publicar
    if (client.connected())
    {
      uint8_t c1 = 0xFF;
      uint8_t c2 = 0xFF;

      // Copia os estados atuais de forma segura usando o Mutex
      if (xSemaphoreTake(mutexEstadosChips, pdMS_TO_TICKS(50)) == pdTRUE)
      {
        c1 = estadoChip1;
        c2 = estadoChip2;
        xSemaphoreGive(mutexEstadosChips);
      }

      // Monta o dicionário JSON com as cores textuais de cada semáforo
      String payload = "{";
      payload += "\"semaforo1\":\"" + obterCorSemaforo(0, c1, c2) + "\",";
      payload += "\"semaforo2\":\"" + obterCorSemaforo(1, c1, c2) + "\",";
      payload += "\"semaforo3\":\"" + obterCorSemaforo(2, c1, c2) + "\",";
      payload += "\"semaforo4\":\"" + obterCorSemaforo(3, c1, c2) + "\",";
      payload += "\"semaforo5\":\"" + obterCorSemaforo(4, c1, c2) + "\"";
      payload += "}";

      // Publica no tópico MQTT configurado
      if (client.publish(topico_sinaleiras, payload.c_str()))
      {
        Serial.println("[MQTT] Estado dos semáforos publicado com sucesso.");
      }
      else
      {
        Serial.println("[MQTT] Falha ao publicar estado.");
      }
    }
  }
}

bool parseBooleanPayload(const String &payload, bool &value)
{
  if (payload == "1" || payload == "true" || payload == "on" || payload == "ligado" || payload == "ativado")
  {
    value = true;
    return true;
  }
  if (payload == "0" || payload == "false" || payload == "off" || payload == "desligado" || payload == "desativado")
  {
    value = false;
    return true;
  }
  return false;
}

void handleCommand(const char *topic, const String &payload)
{
  Serial.printf("[COMMAND] Recebido em %s = %s\n", topic, payload.c_str());

  bool boolValue;
  if (strcmp(topic, topic_amarelo) == 0)
  {
    if (parseBooleanPayload(payload, boolValue))
    {
      xSemaphoreTake(mutexModoAmr, pdMS_TO_TICKS(100));
      modoAmareloPiscante = boolValue;
      xSemaphoreGive(mutexModoAmr);
    }
  }
  else if (strcmp(topic, topic_pedestre) == 0)
  {
    if (parseBooleanPayload(payload, boolValue))
    {
      xSemaphoreTake(mutexPedestre, pdMS_TO_TICKS(100));
      botaoPedestrePressionado = boolValue;
      xSemaphoreGive(mutexPedestre);
    }
  }
}

void conectaWifi()
{
  Serial.println("[WiFi] Conectando a rede: " + String(ssid));
  WiFi.begin(ssid, password);
  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 20)
  {
    delay(500);
    Serial.print(".");
    tentativas++;
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\n[WiFi] Conectado com sucesso! IP: " + WiFi.localIP().toString());
  }
  else
  {
    Serial.println("\n[WiFi] Falha na conexao!");
  }
}

void reconectaMQTT()
{
  while (!client.connected())
  {
    Serial.print("[MQTT] Tentando conexão...");
    if (client.connect(MQTT_ID))
    {
      Serial.println("conectado!");
      client.subscribe(topic_amarelo);
      client.subscribe(topic_pedestre);
    }
    else
    {
      Serial.print("falhou, rc=");
      Serial.print(client.state());
      Serial.println(" tentando novamente em 5 segundos");
      delay(5000);
    }
  }
}

void setup()
{
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  chip1.begin();
  chip2.begin();

  pinMode(PUSH_PIN, INPUT_PULLUP);

  mutexModoAmr = xSemaphoreCreateMutex();
  mutexPedestre = xSemaphoreCreateMutex();
  mutexEstadosChips = xSemaphoreCreateMutex();

  conectaWifi();
  client.setServer(mqtt_server, 1883);
  client.setCallback([](char *topic, byte *payload, unsigned int length)
                     {
    String message;
    message.reserve(length);
    for (unsigned int i = 0; i < length; i++) {
      message += static_cast<char>(payload[i]);
    }
    message.trim();
    message.toLowerCase();
    handleCommand(topic, message); });
  // Cria a tarefa no Core 0, com 4096 bytes de Stack e prioridade 1
  xTaskCreatePinnedToCore(
      taskPublicarSemaforos, // Função que executa a tarefa
      "TaskPublicarMQTT",    // Nome interno da tarefa
      4096,                  // Tamanho da Stack (memória)
      NULL,                  // Parâmetros passados para a tarefa
      1,                     // Prioridade da tarefa (baixa/média)
      NULL,                  // Handle da tarefa (não necessário aqui)
      1                      // Core onde a tarefa vai rodar (Core 1)
  );

  desligarTodosLeds();
  atualizarCruzamento();
  tempoEstado = millis();
}

void loop()
{
  // Mantém a conexão MQTT ativa e processa callbacks
  if (!client.connected())
  {
    reconectaMQTT();
  }
  client.loop();

  // 1. Verificação do Modo Amarelo Piscante
  bool amrAtivo;
  xSemaphoreTake(mutexModoAmr, pdMS_TO_TICKS(10));
  amrAtivo = modoAmareloPiscante;
  xSemaphoreGive(mutexModoAmr);

  if (amrAtivo)
  {
    static unsigned long tempoPisca = 0;
    static bool statusPisca = false;

    if (millis() - tempoPisca >= 1000)
    {
      tempoPisca = millis();
      statusPisca = !statusPisca;
      desligarTodosLeds();
      if (statusPisca)
      {
        for (int i = 0; i < 5; i++)
          setSemaforo(i, COR_AMARELO);
      }
      atualizarCruzamento();
    }
    return;
  }

  // 2. Verificação do Pedestre (Físico ou MQTT)
  bool pedAtivo;
  if (digitalRead(PUSH_PIN) == LOW)
  {
    xSemaphoreTake(mutexPedestre, pdMS_TO_TICKS(10));
    botaoPedestrePressionado = true;
    xSemaphoreGive(mutexPedestre);
  }

  xSemaphoreTake(mutexPedestre, pdMS_TO_TICKS(10));
  pedAtivo = botaoPedestrePressionado;
  xSemaphoreGive(mutexPedestre);

  if (pedAtivo)
  {
    handlePedestre();
    return;
  }

  // 3. Máquina de Estados dos Semáforos (Sem travar com delay)
  unsigned long tempoDecorrido = millis() - tempoEstado;

  switch (estagioAtual)
  {
  case 0: // Estágio Inicial (Verde 1 e 2)
    desligarTodosLeds();
    setSemaforo(0, COR_VERDE);
    setSemaforo(1, COR_VERDE);
    setSemaforo(2, COR_VERMELHO);
    setSemaforo(3, COR_VERMELHO);
    setSemaforo(4, COR_VERMELHO);
    atualizarCruzamento();

    if (tempoDecorrido >= 5000)
    {
      estagioAtual = 1;
      tempoEstado = millis();
      atualizarSupervisorio("Transição: Amarelo no Semáforo 1");
    }
    break;

  case 1: // Transição: Semáforo 1 vai para Amarelo antes de abrir o resto
    desligarTodosLeds();
    setSemaforo(0, COR_AMARELO);
    setSemaforo(1, COR_VERDE);
    setSemaforo(2, COR_VERMELHO);
    setSemaforo(3, COR_VERMELHO);
    setSemaforo(4, COR_VERMELHO);
    atualizarCruzamento();

    if (tempoDecorrido >= 1000)
    { // Dura 1 segundo
      estagioAtual = 2;
      tempoEstado = millis();
      atualizarSupervisorio("Estágio 2: Verde no 2, 3 e 5");
    }
    break;

  case 2: // Estágio Avançado
    desligarTodosLeds();
    setSemaforo(0, COR_VERMELHO);
    setSemaforo(1, COR_VERDE);
    setSemaforo(2, COR_VERDE);
    setSemaforo(3, COR_VERMELHO);
    setSemaforo(4, COR_VERDE);
    atualizarCruzamento();

    if (tempoDecorrido >= 9000)
    { // Completa os 10 segundos totais planejados
      estagioAtual = 3;
      tempoEstado = millis();
      atualizarSupervisorio("Transição: Amarelo no 1, 2 e 3");
    }
    break;

  case 3: // Transição para o Estágio Final
    desligarTodosLeds();
    setSemaforo(0, COR_AMARELO);
    setSemaforo(1, COR_AMARELO);
    setSemaforo(2, COR_AMARELO);
    setSemaforo(3, COR_VERMELHO);
    setSemaforo(4, COR_VERDE);
    atualizarCruzamento();

    if (tempoDecorrido >= 1000)
    { // Dura 1 segundo
      estagioAtual = 4;
      tempoEstado = millis();
      atualizarSupervisorio("Estágio 3: Verde no 4 e 5");
    }
    break;

  case 4: // Estágio Final
    desligarTodosLeds();
    setSemaforo(0, COR_VERMELHO);
    setSemaforo(1, COR_VERMELHO);
    setSemaforo(2, COR_VERMELHO);
    setSemaforo(3, COR_VERDE);
    setSemaforo(4, COR_VERDE);
    atualizarCruzamento();

    if (tempoDecorrido >= 9000)
    {
      estagioAtual = 0;
      tempoEstado = millis();
      atualizarSupervisorio("Retornando ao Estágio 1");
    }
    break;
  }
}