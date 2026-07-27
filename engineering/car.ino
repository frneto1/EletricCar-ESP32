#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>

// --- WIFI ---
const char* ssid     = "CARRO_ESP32";
const char* password = "1234";

// --- UDP ---
WiFiUDP udp;
unsigned int localPort = 1234;
char packetBuffer[255];

// --- WEBSOCKET ---
WebSocketsServer ws = WebSocketsServer(81);

// ============================================================
// L298N — TRAÇÃO (motores traseiros)
// ============================================================
#define IN1 26
#define IN2 27
#define ENA 14

#define IN3 33
#define IN4 32
#define ENB 25

// ============================================================
// MX1508 — DIREÇÃO (motores dianteiros, independentes)
// ============================================================
#define MX_IN1 18  // dianteiro esquerdo +
#define MX_IN2 19  // dianteiro esquerdo -
#define MX_IN3 21  // dianteiro direito +
#define MX_IN4 22  // dianteiro direito -

// ============================================================
// DRS — ASA MÓVEL (2x SG90)
// ============================================================
#define PINO_SERVO_ASA_DIANT 4
#define PINO_SERVO_ASA_TRAS  5

Servo servoAsaDiant;
Servo servoAsaTras;

const int ASA_FECHADA = 90;   // ângulo com a asa fechada (modo normal)
const int ASA_ABERTA  = 30;   // ângulo com a asa aberta (modo DRS)

bool drsAtivo = false;

// --- ESTADO ---
unsigned long ultimoPacote = 0;
unsigned long ultimoWS     = 0;
int ultimoT = 0;
int ultimoS = 0;
int ultimoDRS = 0;

// ============================================================
// PWM
// ============================================================
void configurarPWM() {
  ledcAttach(ENA, 1000, 8);
  ledcAttach(ENB, 1000, 8);

  ledcAttach(MX_IN1, 1000, 8);
  ledcAttach(MX_IN2, 1000, 8);
  ledcAttach(MX_IN3, 1000, 8);
  ledcAttach(MX_IN4, 1000, 8);
}

void escreverPWM(int pin, int valor) {
  ledcWrite(pin, valor);
}

// ============================================================
// DRS — controle das asas
// ============================================================
void aplicarDRS(bool ativo) {
  int angulo = ativo ? ASA_ABERTA : ASA_FECHADA;
  servoAsaDiant.write(angulo);
  servoAsaTras.write(angulo);
}

// ============================================================
// MOTOR DIANTEIRO DIREITO (MX1508 canal A)
// ============================================================
void motorDianteiroEsq(int valor) {
   Serial.println(valor);
  valor = constrain(valor, -255, 255);
  if (valor > 0) {
    escreverPWM(MX_IN1, valor);
    escreverPWM(MX_IN2, 0);
  } else if (valor < 0) {
    escreverPWM(MX_IN1, 0);
    escreverPWM(MX_IN2, abs(valor));
  } else {
    escreverPWM(MX_IN1, 0);
    escreverPWM(MX_IN2, 0);
  }
}

// ============================================================
// MOTOR DIANTEIRO ESQUERDO (MX1508 canal B)
// ============================================================
void motorDianteiroDir(int valor) {
  valor = constrain(valor, -255, 255);
  if (valor > 0) {
    escreverPWM(MX_IN3, valor);
    escreverPWM(MX_IN4, 0);
  } else if (valor < 0) {
    escreverPWM(MX_IN3, 0);
    escreverPWM(MX_IN4, abs(valor));
  } else {
    escreverPWM(MX_IN3, 0);
    escreverPWM(MX_IN4, 0);
  }
}
// ============================================================
// MOTORES TRASEIROS (L298N) — tração
// ============================================================
void tracaoTraseira(int T) {
  int pwm = map(abs(T), 0, 100, 0, 180);
  if (T > 0) {
    digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  } else if (T < 0) {
    digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  } else {
    digitalWrite(IN1, LOW);  digitalWrite(IN2, LOW);
    pwm = 0;
  }
  escreverPWM(ENA, pwm);

  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
  escreverPWM(ENB, 0);
}

// ============================================================
// LÓGICA PRINCIPAL — tração + direção diferencial
// ============================================================
void moverMotores(int T, int S, int comTracao) {
  if (comTracao && abs(T) != 0) {
    tracaoTraseira(T);
  } else {
    tracaoTraseira(0);
  }

  int pwmBase = map(abs(T), 0, 100, 0, 255);
  int pwmGiro = map(abs(S), 0, 100, 0, 255);

  if (comTracao && abs(T) != 0) {
    int sinalT = (T > 0) ? 1 : -1;
    int velEsq = sinalT * pwmBase;
    int velDir = sinalT * pwmBase;

    if (abs(S) > 10) {
      int ajuste = map(abs(S), 0, 100, 0, pwmBase);
      if (S > 0) {
        velDir = sinalT * (pwmBase - ajuste);
      } else {
        velEsq = sinalT * (pwmBase - ajuste);
      }
    }

    motorDianteiroEsq(velEsq);
    motorDianteiroDir(velDir);

  } else if (abs(S) > 10) {
    if (S > 0) {
      motorDianteiroEsq(pwmGiro);
      motorDianteiroDir(-pwmGiro);
    } else {
      motorDianteiroEsq(-pwmGiro);
      motorDianteiroDir(pwmGiro);
    }
  } else {
    motorDianteiroEsq(0);
    motorDianteiroDir(0);
  }
}

// ============================================================
// WEBSOCKET
// ============================================================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    Serial.printf("[WS] Cliente %u conectado\n", num);
  } else if (type == WStype_DISCONNECTED) {
    Serial.printf("[WS] Cliente %u desconectado\n", num);
  }
}

// ============================================================
// DASHBOARD
// ============================================================
void enviarDashboard() {
  if (millis() - ultimoWS < 100) return;
  ultimoWS = millis();

  int thr = ultimoT > 0 ? ultimoT : 0;
  int brk = ultimoT < 0 ? abs(ultimoT) : 0;

  StaticJsonDocument<200> doc;
  doc["servo"] = map(ultimoS, -100, 100, 40, 140);
  doc["thr"]   = thr;
  doc["brk"]   = brk;
  doc["spd"]   = map(thr, 0, 100, 0, 60);
  doc["rpm"]   = map(thr, 0, 100, 0, 3000);
  doc["enc"]   = 0;
  doc["bat"]   = 12.0;
  doc["drs"]   = drsAtivo ? 1 : 0;

  String json;
  serializeJson(doc, json);
  ws.broadcastTXT(json);
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);

  WiFi.softAP(ssid, password);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());

  udp.begin(localPort);
  Serial.println("UDP na porta 1234");

  ws.begin();
  ws.onEvent(webSocketEvent);
  Serial.println("WebSocket na porta 81");

  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  configurarPWM();

  // Servos DRS
  servoAsaDiant.attach(PINO_SERVO_ASA_DIANT, 500, 2400);
  servoAsaTras.attach(PINO_SERVO_ASA_TRAS, 500, 2400);
  aplicarDRS(false);  // começa fechado

  moverMotores(0, 0, 0);
  Serial.println("Pronto! L298N + MX1508 + DRS (2 servos)");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  ws.loop();

  int packetSize = udp.parsePacket();
  if (packetSize) {
    int len = udp.read(packetBuffer, 255);
    if (len > 0) packetBuffer[len] = 0;

    int T = 0, S = 0, TRACAO = 0, DRS = 0;
    if (sscanf(packetBuffer, "T:%d S:%d TRACAO:%d DRS:%d", &T, &S, &TRACAO, &DRS) == 4) {
      ultimoPacote = millis();
      ultimoT = T;
      ultimoS = S;
      moverMotores(T, S, TRACAO);

      // DRS só reage na transição (borda de subida) — toggle já tratado no Python
      if (DRS != ultimoDRS) {
        drsAtivo = DRS == 1;
        aplicarDRS(drsAtivo);
        Serial.println(drsAtivo ? "DRS: ABERTO" : "DRS: FECHADO");
        ultimoDRS = DRS;
      }
    }
  }

  // FAILSAFE — motores parados, mas DRS mantém posição atual
  if (millis() - ultimoPacote > 500) {
    moverMotores(0, 0, 0);
    ultimoT = 0;
    ultimoS = 0;
  }

  enviarDashboard();
}
