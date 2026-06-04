#include <Arduino.h>
#include <Wire.h>
#include <PCF8574.h>

#define CHIP_UM 0x38
#define CHIP_DOIS 0x39

#define PUSH_PIN 4
#define SDA_PIN 21
#define SCL_PIN 22

// semaforo 1 ta verde 0x39 0b11111101, amarelo 0x39 0b11111110, vermelho 0x38 0b01111111
// semaforo 2 ta verde 0x38 0b10111111, amarelo 0x38 0b11011111, vermelho 0x38 0b11101111
// semaforo 3 ta verde 0x38 0b11110111, amarelo 0x38 0b11111011, vermelho 0x38 0b11111101
// semaforo 4 ta verde 0x39 0b11101111, amarelo 0x39 0b11110111, vermelho 0x39 0b11111011
// semaforo 5 ta verde 0x39 0b01111111, amarelo 0x39 0b10111111, vermelho 0x39 0b11011111

// [p7 ... p0]

PCF8574 chip1(0x38);
PCF8574 chip2(0x39);

uint8_t estadoChip1 = 0xFF;
uint8_t estadoChip2 = 0xFF;

// Variáveis de controle de tempo e estados
unsigned long tempoEstado = 0;
int estagioAtual = 0;
bool modoAmareloPiscante = false;
bool botaoPedestrePressionado = false;

// Estrutura para mapear um estado específico (cor)
typedef struct
{
  uint8_t chip;    // Endereço I2C: 0x38 (chipUM) ou 0x39 (chipDOIS)
  uint8_t comando; // Máscara de bits (ex: 0b11111101)
} EstadoSemaforo;

// Estrutura principal do Semáforo
typedef struct
{
  int id;
  EstadoSemaforo verde;
  EstadoSemaforo amarelo;
  EstadoSemaforo vermelho;
} Semaforo;

const Semaforo semaforos[] = {
    // Semáforo 1
    {1, {CHIP_DOIS, 0b11111101}, {CHIP_DOIS, 0b11111110}, {CHIP_UM, 0b01111111}},
    // Semáforo 2
    {2, {CHIP_UM, 0b10111111}, {CHIP_UM, 0b11011111}, {CHIP_UM, 0b11101111}},
    // Semáforo 3
    {3, {CHIP_UM, 0b11110111}, {CHIP_UM, 0b11111011}, {CHIP_UM, 0b11111101}},
    // Semáforo 4
    {4, {CHIP_DOIS, 0b11101111}, {CHIP_DOIS, 0b11110111}, {CHIP_DOIS, 0b11111011}},
    // Semáforo 5
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
  estadoChip1 = 0xFF;
  estadoChip2 = 0xFF;
}

void prepararLed(EstadoSemaforo estado)
{
  if (estado.chip == CHIP_UM)
  {
    estadoChip1 &= estado.comando;
  }
  else if (estado.chip == CHIP_DOIS)
  {
    estadoChip2 &= estado.comando;
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

  delay(1000);

  desligarTodosLeds();
  for (int i = 0; i < 5; i++)
  {
    setSemaforo(i, COR_VERMELHO);
  }
  atualizarCruzamento();

  do
  {
    delay(50);
  } while (digitalRead(PUSH_PIN) == LOW);

  tempoEstado = millis();
}

void setup()
{
  Serial.begin(115200);
  Wire.begin(21, 22);
  chip1.begin();
  chip2.begin();

  pinMode(PUSH_PIN, INPUT_PULLUP);

  desligarTodosLeds();
  atualizarCruzamento();
  tempoEstado = millis();
}

void loop()
{
  // escreveByte(0x38, 0x55);
  // escreveByte(0x39, 0xAA);
  // delay(1000);
  // escreveByte(0x38, 0xAA);
  // escreveByte(0x39, 0x55);
  // delay(1000);

  // add codigo pro modo amarelo piscante

  if (modoAmareloPiscante)
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
      Serial.println("ALERTA: Modo Amarelo Piscante Ativo");
    }
    return; // Ignora o fluxo normal se estiver piscando
  }

  unsigned long tempoDecorrido = millis() - tempoEstado;

  if (digitalRead(PUSH_PIN) == LOW)
  {
    handlePedestre();
    return;
  }

  switch (estagioAtual)
  {
  case 0: // Saturnino de Brito
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
      atualizarSupervisorio("Tempo 1 -> 2");
    }
    break;
  case 1:
    desligarTodosLeds();
    setSemaforo(0, COR_AMARELO);
    setSemaforo(1, COR_VERDE);
    setSemaforo(2, COR_VERMELHO);
    setSemaforo(3, COR_VERMELHO);
    setSemaforo(4, COR_VERMELHO);
    atualizarCruzamento();

    delay(1000);

    desligarTodosLeds();
    setSemaforo(0, COR_VERMELHO);
    setSemaforo(1, COR_VERDE);
    setSemaforo(2, COR_VERDE);
    setSemaforo(3, COR_VERMELHO);
    setSemaforo(4, COR_VERDE);
    atualizarCruzamento();

    if (tempoDecorrido >= 10000)
    {
      estagioAtual = 2;
      tempoEstado = millis();
      atualizarSupervisorio("Tempo 2 -> 3");
    }
    break;
  case 2:
    desligarTodosLeds();
    setSemaforo(0, COR_AMARELO);
    setSemaforo(1, COR_AMARELO);
    setSemaforo(2, COR_AMARELO);
    setSemaforo(3, COR_VERMELHO);
    setSemaforo(4, COR_VERDE);
    atualizarCruzamento();

    delay(1000);

    desligarTodosLeds();
    setSemaforo(0, COR_VERMELHO);
    setSemaforo(1, COR_VERMELHO);
    setSemaforo(2, COR_VERMELHO);
    setSemaforo(3, COR_VERDE);
    setSemaforo(4, COR_VERDE);
    atualizarCruzamento();

    if (tempoDecorrido >= 10000)
    {
      estagioAtual = 0;
      tempoEstado = millis();
      atualizarSupervisorio("Tempo 3 -> 1");
    }
    break;
  }
}