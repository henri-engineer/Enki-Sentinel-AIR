/****************************************************
 * MONITOR ENKI - Sistema Híbrido de Controle de Qualidade do Ar
 * ESP32 + MQ-2 + DHT22 + LCD + Matriz WS2812 + MQTT
 * Versão Otimizada para Wokwi - API ESP32 3.x
 ****************************************************/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHTesp.h>
#include <Stepper.h>

// ---------------------------------------------------
// DEFINIÇÕES DE HARDWARE
// ---------------------------------------------------
#define MQ2_PIN            36    // VP - ADC1
#define DHT_PIN            15

#define SERVO_PIN          18    // Servo
#define SERVO_FREQ         50
#define SERVO_RESOLUTION   8

#define FAN_PIN            16    // LED Ventilador
#define EXHAUST_PIN        19    // Relé

#define LED_PIN            5     // NeoPixel
#define NUM_LEDS           256   // 8x32

#define STEPPER_PIN1 25
#define STEPPER_PIN2 26
#define STEPPER_PIN3 32
#define STEPPER_PIN4 33

#define BUZZER_PIN 27

// Limites ADC
#define LIMITE_SEGURO 3760
#define LIMITE_ATENCAO 3840

// ---------------------------------------------------
// OBJETOS
// ---------------------------------------------------
LiquidCrystal_I2C lcd(0x27, 20, 4);
Adafruit_NeoPixel matrix(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
DHTesp dht;
Stepper stepperFiltro(2048, STEPPER_PIN1, STEPPER_PIN3, STEPPER_PIN2, STEPPER_PIN4);

WiFiClient espClient;
PubSubClient mqtt(espClient);

// ---------------------------------------------------
// VARIÁVEIS GLOBAIS
// ---------------------------------------------------
int posicaoJanela = 0;
int posicaoFiltro = 0;
bool exaustorLigado = false;
int velocidadeVentilador = 0;

unsigned long ultimoEnvio = 0;
unsigned long ultimaLeitura = 0;
unsigned long ultimaTentativaMQTT = 0;

bool wifiOK = false;
bool mqttOK = false;
bool dhtOK = false;

String statusAtual = "INICIALIZANDO";

// ---------------------------------------------------
// WI-FI E MQTT (com fallback)
// ---------------------------------------------------
const char* ssid = "Wokwi-GUEST";
const char* password = "";
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* mqtt_topic = "enki/monitor/co";

// ---------------------------------------------------
// PADRÃO SOS PARA MATRIZ
// ---------------------------------------------------
const byte S[8][8] = {
  {0,1,1,1,1,1,0,0}, {1,1,0,0,0,1,1,0}, {1,1,0,0,0,0,0,0}, {0,1,1,1,1,0,0,0},
  {0,0,0,0,1,1,0,0}, {0,0,0,0,0,1,1,0}, {1,1,0,0,0,1,1,0}, {0,1,1,1,1,1,0,0}
};

const byte O[8][8] = {
  {0,1,1,1,1,1,0,0}, {1,1,0,0,0,1,1,0}, {1,1,0,0,0,1,1,0}, {1,1,0,0,0,1,1,0},
  {1,1,0,0,0,1,1,0}, {1,1,0,0,0,1,1,0}, {1,1,0,0,0,1,1,0}, {0,1,1,1,1,1,0,0}
};

// ---------------------------------------------------
// FUNÇÕES MATRIZ LED
// ---------------------------------------------------
void setMatrixColor(uint32_t color) {
  for (int i = 0; i < NUM_LEDS; i++) {
    matrix.setPixelColor(i, color);
  }
  matrix.show();
}

void drawLetter(const byte letter[8][8], int offsetX, uint32_t color) {
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      int x = offsetX + col;
      if (x >= 0 && x < 32) {
        int pixelIndex = row * 32 + x;
        if (letter[row][col] == 1) {
          matrix.setPixelColor(pixelIndex, color);
        }
      }
    }
  }
}

void showSOS(uint32_t color) {
  matrix.clear();
  drawLetter(S, 2, color);
  drawLetter(O, 12, color);
  drawLetter(S, 22, color);
  matrix.show();
}

// ---------------------------------------------------
// FUNÇÕES DE AUTOMAÇÃO
// ---------------------------------------------------
void controlarJanela(int angulo) {
  posicaoJanela = constrain(angulo, 0, 180);
  int duty = map(posicaoJanela, 0, 180, 13, 51); // 5-10% duty @ 8-bit
  ledcWrite(SERVO_PIN, duty);
  Serial.print("🪟 Janela: ");
  Serial.print(posicaoJanela);
  Serial.println("°");
}

void controlarExaustor(bool ligar) {
  exaustorLigado = ligar;
  digitalWrite(EXHAUST_PIN, ligar ? HIGH : LOW);
  Serial.print("💨 Exaustor: ");
  Serial.println(ligar ? "ON" : "OFF");
}

void controlarVentilador(int vel) {
  velocidadeVentilador = constrain(vel, 0, 255);
  analogWrite(FAN_PIN, velocidadeVentilador);
  Serial.print("🌀 Ventilador: ");
  Serial.print((velocidadeVentilador * 100) / 255);
  Serial.println("%");
}

void moverFiltro(int novaPos) {
  int steps = novaPos - posicaoFiltro;
  if (steps != 0) {
    stepperFiltro.step(steps);
    posicaoFiltro = novaPos;
    Serial.print("🔄 Filtro pos: ");
    Serial.println(posicaoFiltro);
  }
}

void aplicarAutomacao(int adc) {
  if (adc <= LIMITE_SEGURO) {
    // MODO SEGURO
    statusAtual = "SEGURO";
    setMatrixColor(matrix.Color(0, 255, 0));
    controlarJanela(0);
    controlarExaustor(false);
    controlarVentilador(0);
    moverFiltro(0);
    digitalWrite(BUZZER_PIN, LOW);
    
  } else if (adc > LIMITE_SEGURO && adc <= LIMITE_ATENCAO) {
    // MODO ATENÇÃO
    statusAtual = "ATENCAO";
    setMatrixColor(matrix.Color(255, 255, 0));
    controlarJanela(90);
    controlarExaustor(false);
    controlarVentilador(128);
    moverFiltro(512);
    digitalWrite(BUZZER_PIN, LOW);
    
  } else {
    // MODO PERIGO
    statusAtual = "PERIGO";
    controlarJanela(180);
    controlarExaustor(true);
    controlarVentilador(255);
    moverFiltro(1024);
    
    // Animação SOS
    for (int i = 0; i < 2; i++) {
      setMatrixColor(matrix.Color(255, 0, 0));
      digitalWrite(BUZZER_PIN, HIGH);
      delay(300);
      showSOS(matrix.Color(255, 0, 0));
      digitalWrite(BUZZER_PIN, LOW);
      delay(300);
    }
  }
}

// ---------------------------------------------------
// FUNÇÕES WiFi/MQTT (com tratamento de erro)
// ---------------------------------------------------
void setupWiFi() {
  Serial.print("\n→ WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 20) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiOK = true;
    Serial.println(" ✓");
    Serial.print("  IP: ");
    Serial.println(WiFi.localIP());
  } else {
    wifiOK = false;
    Serial.println(" ✗ FALHA (modo offline)");
  }
}

bool reconnectMQTT() {
  if (!wifiOK) return false;
  if (millis() - ultimaTentativaMQTT < 5000) return false;
  
  ultimaTentativaMQTT = millis();
  Serial.print("→ MQTT...");
  
  String clientId = "ENKI-" + String(random(0xffff), HEX);
  
  if (mqtt.connect(clientId.c_str())) {
    mqttOK = true;
    Serial.println(" ✓");
    return true;
  } else {
    mqttOK = false;
    Serial.print(" ✗ (");
    Serial.print(mqtt.state());
    Serial.println(")");
    return false;
  }
}

void publicarMQTT(int adc, float temp, float hum) {
  if (!mqttOK) return;
  
  StaticJsonDocument<256> doc;
  doc["device"] = "MonitorENKI";
  doc["adc"] = adc;
  doc["status"] = statusAtual;
  doc["temp"] = temp;
  doc["umid"] = hum;
  doc["automacao"]["janela"] = posicaoJanela;
  doc["automacao"]["exaustor"] = exaustorLigado;
  doc["automacao"]["ventilador"] = (velocidadeVentilador * 100) / 255;
  doc["automacao"]["filtro"] = posicaoFiltro;
  
  char buffer[256];
  serializeJson(doc, buffer);
  
  if (mqtt.publish(mqtt_topic, buffer)) {
    Serial.println("✓ MQTT enviado");
  } else {
    Serial.println("✗ MQTT falhou");
    mqttOK = false;
  }
}

// ---------------------------------------------------
// ATUALIZAR LCD
// ---------------------------------------------------
void atualizarLCD(int adc, float temp, float hum) {
  lcd.clear();
  
  lcd.setCursor(1, 0);
  lcd.print("MONITOR ENKI v2.0");
  
  lcd.setCursor(0, 1);
  lcd.print("ADC:");
  lcd.print(adc);
  lcd.print(" ST:");
  lcd.print(statusAtual.substring(0, 3));
  
  lcd.setCursor(0, 2);
  lcd.print("T:");
  lcd.print(temp, 1);
  lcd.print("C H:");
  lcd.print(hum, 1);
  lcd.print("%");
  
  lcd.setCursor(0, 3);
  lcd.print("J:");
  lcd.print(posicaoJanela);
  lcd.print(" E:");
  lcd.print(exaustorLigado ? "ON " : "OFF");
  lcd.print(" V:");
  lcd.print((velocidadeVentilador * 100) / 255);
  lcd.print("%");
}

// ---------------------------------------------------
// SETUP
// ---------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n╔════════════════════════════════╗");
  Serial.println("║   MONITOR ENKI v2.0 HÍBRIDO   ║");
  Serial.println("╚════════════════════════════════╝");
  
  // Pinos
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(EXHAUST_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(MQ2_PIN, INPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(EXHAUST_PIN, LOW);
  
  // LCD
  Serial.print("→ LCD...");
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(2, 1);
  lcd.print("MONITOR ENKI v2.0");
  lcd.setCursor(3, 2);
  lcd.print("Inicializando...");
  Serial.println(" ✓");
  
  // DHT22
  Serial.print("→ DHT22...");
  dht.setup(DHT_PIN, DHTesp::DHT22);
  delay(2000);
  Serial.println(" ✓");
  
  // Matriz
  Serial.print("→ Matriz LED...");
  matrix.begin();
  matrix.setBrightness(50);
  matrix.clear();
  matrix.show();
  Serial.println(" ✓");
  
  // Servo - NOVA API ESP32 3.x
  Serial.print("→ Servo...");
  ledcAttach(SERVO_PIN, SERVO_FREQ, SERVO_RESOLUTION);
  controlarJanela(0);
  Serial.println(" ✓");
  
  // Motor de Passo
  Serial.print("→ Stepper...");
  stepperFiltro.setSpeed(10);
  Serial.println(" ✓");
  
  // WiFi (com fallback)
  setupWiFi();
  
  // MQTT
  if (wifiOK) {
    mqtt.setServer(mqtt_server, mqtt_port);
    reconnectMQTT();
  }
  
  Serial.println("\n=== Sistema Iniciado ===");
  Serial.println("Modo: " + String(wifiOK ? "ONLINE" : "OFFLINE"));
  Serial.println();
  
  delay(2000);
  lcd.clear();
}

// ---------------------------------------------------
// LOOP PRINCIPAL
// ---------------------------------------------------
void loop() {
  unsigned long agora = millis();
  
  // Gestão de conectividade (não-bloqueante)
  if (wifiOK) {
    if (WiFi.status() != WL_CONNECTED) {
      wifiOK = false;
      mqttOK = false;
      Serial.println("✗ WiFi perdido");
    } else {
      if (!mqtt.connected()) {
        reconnectMQTT();
      } else {
        mqtt.loop();
      }
    }
  }
  
  // Leitura de sensores (a cada 3s)
  if (agora - ultimaLeitura >= 3000) {
    ultimaLeitura = agora;
    
    // Lê MQ-2
    int adc = analogRead(MQ2_PIN);
    
    // Lê DHT22
    TempAndHumidity d = dht.getTempAndHumidity();
    float temp = isnan(d.temperature) ? 0 : d.temperature;
    float hum = isnan(d.humidity) ? 0 : d.humidity;
    
    // Log
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.print("ADC: ");
    Serial.print(adc);
    Serial.print(" | T: ");
    Serial.print(temp, 1);
    Serial.print("°C | H: ");
    Serial.print(hum, 1);
    Serial.println("%");
    
    // Aplica automação
    aplicarAutomacao(adc);
    
    // Atualiza LCD
    atualizarLCD(adc, temp, hum);
    
    // Publica MQTT (se online)
    if (mqttOK && (agora - ultimoEnvio >= 5000)) {
      publicarMQTT(adc, temp, hum);
      ultimoEnvio = agora;
    }
  }
  
  delay(100);
}
