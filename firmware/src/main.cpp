/*
 * Actividad 4 - Sistema de control de iluminación basado en gestos de la mano
 * Microcontroladores - UMNG
 *
 * El PC (Python + MediaPipe Gesture Recognizer) reconoce el gesto con la
 * webcam y envía UN carácter por USB-Serial (115200 baudios):
 *
 *   'F'  Closed_Fist  (puño)          -> LED amarillo al 30 %
 *   'V'  Victory      (amor y paz)    -> LED azul al 70 %
 *   'P'  Open_Palm    (mano abierta)  -> LED rojo al 100 %
 *   'D'  Thumb_Down   (pulgar abajo)  -> Interrupción 1: secuencia Modo 1
 *   'U'  Thumb_Up     (pulgar arriba) -> Interrupción 2: secuencia Modo 2
 *   'X'  (manual)                     -> apaga todo
 *
 * La recepción serial es por interrupción: el callback Serial.onReceive() se
 * dispara cuando llega un byte por la UART, guarda el comando y el loop()
 * lo atiende de inmediato. Las secuencias no usan delay(), así que un gesto
 * nuevo corta la secuencia en el mismo instante.
 * Si no hay mano o el gesto no es válido, el PC no envía nada y el ESP32
 * mantiene el último estado.
 */

#include <Arduino.h>

// ---------------- Pines (ESP32 DevKit 30 pines) ----------------
const uint8_t PIN_AMARILLO = 25;
const uint8_t PIN_AZUL     = 26;
const uint8_t PIN_ROJO     = 27;

// ---------------- PWM (LEDC) ----------------
const uint32_t PWM_FREQ = 5000;  // Hz
const uint8_t  PWM_RES  = 8;     // bits -> duty 0..255
const uint8_t  CH_AMARILLO = 0, CH_AZUL = 1, CH_ROJO = 2;  // solo core 2.x

// Duty cycle de cada intensidad (8 bits)
const uint8_t DUTY_30  = 77;   // 0.30 * 255
const uint8_t DUTY_70  = 179;  // 0.70 * 255
const uint8_t DUTY_100 = 255;

// Compatibilidad con Arduino-ESP32 core 2.x y 3.x (cambió la API de LEDC)
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  void pwmInit(uint8_t pin, uint8_t) { ledcAttach(pin, PWM_FREQ, PWM_RES); }
  void pwmWrite(uint8_t pin, uint8_t, uint8_t duty) { ledcWrite(pin, duty); }
#else
  void pwmInit(uint8_t pin, uint8_t ch) { ledcSetup(ch, PWM_FREQ, PWM_RES); ledcAttachPin(pin, ch); }
  void pwmWrite(uint8_t, uint8_t ch, uint8_t duty) { ledcWrite(ch, duty); }
#endif

void setLeds(uint8_t amarillo, uint8_t azul, uint8_t rojo) {
  pwmWrite(PIN_AMARILLO, CH_AMARILLO, amarillo);
  pwmWrite(PIN_AZUL,     CH_AZUL,     azul);
  pwmWrite(PIN_ROJO,     CH_ROJO,     rojo);
}

// ---------------- Máquina de estados ----------------
enum Modo { APAGADO, AMARILLO_30, AZUL_70, ROJO_100, SECUENCIA_1, SECUENCIA_2 };
Modo modoActual = APAGADO;

volatile char comandoPendiente = 0;  // lo escribe el callback de la UART

// Callback de recepción UART: el driver lo ejecuta cuando la interrupción de
// RX de la UART0 avisa que llegaron datos (no hay que preguntar en el loop).
void alRecibirSerial() {
  while (Serial.available()) {
    char c = toupper(Serial.read());
    if (c == 'F' || c == 'V' || c == 'P' || c == 'D' || c == 'U' || c == 'X') {
      comandoPendiente = c;  // nos quedamos con el último comando válido
    }
  }
}

// Variables de las secuencias (no bloqueantes)
unsigned long tInicioSecuencia = 0;
unsigned long tUltimoPaso = 0;
uint8_t pasoSecuencia = 0;

void cambiarModo(Modo nuevo) {
  if (nuevo == modoActual) return;  // mismo gesto repetido: no reinicia nada
  modoActual = nuevo;
  pasoSecuencia = 0;
  tInicioSecuencia = tUltimoPaso = millis();

  switch (modoActual) {
    case AMARILLO_30: setLeds(DUTY_30, 0, 0);  Serial.println("OK: Puno -> Amarillo 30%");        break;
    case AZUL_70:     setLeds(0, DUTY_70, 0);  Serial.println("OK: Victoria -> Azul 70%");        break;
    case ROJO_100:    setLeds(0, 0, DUTY_100); Serial.println("OK: Palma -> Rojo 100%");          break;
    case SECUENCIA_1: setLeds(0, 0, 0);        Serial.println("OK: Pulgar abajo -> INT1 Modo 1"); break;
    case SECUENCIA_2: setLeds(0, 0, 0);        Serial.println("OK: Pulgar arriba -> INT2 Modo 2");break;
    case APAGADO:     setLeds(0, 0, 0);        Serial.println("OK: Apagado");                     break;
  }
}

// Modo 1 (pulgar abajo): "carrera" Amarillo -> Azul -> Rojo -> Azul -> ...
void secuenciaModo1() {
  const unsigned long PASO_MS = 200;
  const uint8_t patron[4][3] = {
    {DUTY_100, 0, 0},
    {0, DUTY_100, 0},
    {0, 0, DUTY_100},
    {0, DUTY_100, 0},
  };
  if (millis() - tUltimoPaso >= PASO_MS) {
    tUltimoPaso = millis();
    setLeds(patron[pasoSecuencia][0], patron[pasoSecuencia][1], patron[pasoSecuencia][2]);
    pasoSecuencia = (pasoSecuencia + 1) % 4;
  }
}

// Modo 2 (pulgar arriba): "ola de respiración", cada LED sube y baja su
// brillo con una senoidal desfasada 120° respecto al anterior.
void secuenciaModo2() {
  const unsigned long ACTUALIZAR_MS = 10;
  const float PERIODO_MS = 1500.0f;
  if (millis() - tUltimoPaso >= ACTUALIZAR_MS) {
    tUltimoPaso = millis();
    float fase = TWO_PI * (float)(millis() - tInicioSecuencia) / PERIODO_MS;
    uint8_t a = (uint8_t)(127.5f * (1.0f + sinf(fase)));
    uint8_t b = (uint8_t)(127.5f * (1.0f + sinf(fase - TWO_PI / 3.0f)));
    uint8_t r = (uint8_t)(127.5f * (1.0f + sinf(fase - 2.0f * TWO_PI / 3.0f)));
    setLeds(a, b, r);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.onReceive(alRecibirSerial);  // habilita la interrupción de RX

  pwmInit(PIN_AMARILLO, CH_AMARILLO);
  pwmInit(PIN_AZUL,     CH_AZUL);
  pwmInit(PIN_ROJO,     CH_ROJO);
  setLeds(0, 0, 0);

  // Prueba rápida de LEDs al arrancar
  setLeds(DUTY_100, 0, 0); delay(250);
  setLeds(0, DUTY_100, 0); delay(250);
  setLeds(0, 0, DUTY_100); delay(250);
  setLeds(0, 0, 0);

  Serial.println();
  Serial.println("=== Actividad 4: Control de iluminacion por gestos ===");
  Serial.println("Comandos: F=30% amarillo, V=70% azul, P=100% rojo, D=Modo 1, U=Modo 2, X=apagar");
  Serial.println("LISTO");
}

void loop() {
  // 1) Atender el comando que dejó la interrupción
  if (comandoPendiente) {
    char c = comandoPendiente;
    comandoPendiente = 0;
    switch (c) {
      case 'F': cambiarModo(AMARILLO_30); break;
      case 'V': cambiarModo(AZUL_70);     break;
      case 'P': cambiarModo(ROJO_100);    break;
      case 'D': cambiarModo(SECUENCIA_1); break;
      case 'U': cambiarModo(SECUENCIA_2); break;
      case 'X': cambiarModo(APAGADO);     break;
    }
  }

  // 2) Avanzar la secuencia activa (sin delay, se interrumpe al instante)
  if (modoActual == SECUENCIA_1) secuenciaModo1();
  else if (modoActual == SECUENCIA_2) secuenciaModo2();
}
