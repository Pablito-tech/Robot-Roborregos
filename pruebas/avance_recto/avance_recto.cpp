/*
 * ============================================================================
 *  Roborregos - PRUEBA DE AVANCE RECTO
 * ============================================================================
 *  Robot diferencial: 2 motores JGA25-371 (200 RPM, con encoder) + rueda loca,
 *  ESP32 de 38 pines, driver TB6612, giroscopio MPU6050 y ultrasonicos.
 *
 *  El robot avanza en linea recta una distancia exacta (p. ej. una celda de
 *  30 cm) combinando tres fuentes de informacion:
 *
 *    1. ENCODERS     -> cuanto avanzo cada rueda (control de distancia, 200 Hz)
 *    2. MPU6050      -> hacia donde apunta el robot (mantener el rumbo recto)
 *    3. ULTRASONICOS -> distancia absoluta a la pared de enfrente y de atras.
 *                       Corrigen el punto de frenado en pleno movimiento y
 *                       verifican el avance real al terminar.
 *
 *  Uso rapido (detalles en README.md):
 *    - Monitor serie a 115200 baudios, terminador "Nueva linea".
 *    - Boton BOOT de la placa  -> avanza 1 celda (30 cm).
 *    - Comandos: a300 (avanza 300 mm), c1 (1 celda), m/e/g/u/k (pruebas),
 *      z (rumbo = 0), r (registro CSV), h (ayuda).
 *    - Cualquier tecla o el boton BOOT durante el movimiento lo detienen.
 *
 *  Requisitos: placa "esp32" de Espressif v3.x en el IDE de Arduino.
 *  No necesita librerias externas.
 * ============================================================================
 */
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "driver/pulse_cnt.h"   // contador de pulsos por hardware (PCNT)

// ============================================================================
//  PINES (misma asignacion que la declaracion general del robot)
// ============================================================================
// ---------- Driver TB6612 ----------
// Motor A (derecho)
const uint8_t PWMA = 25;   // A9
const uint8_t AIN1 = 26;   // A10
const uint8_t AIN2 = 27;   // A11
// Motor B (izquierdo)
const uint8_t PWMB = 13;   // A15
const uint8_t BIN1 = 32;   // A7
const uint8_t BIN2 = 33;   // A8
const uint8_t STBY = 14;   // A12

// ---------- Encoders ----------
const uint8_t ENC_A_C1 = 34;   // A5  (motor A)
const uint8_t ENC_A_C2 = 35;   // A6
const uint8_t ENC_B_C1 = 36;   // A3  (motor B, VP)
const uint8_t ENC_B_C2 = 39;   // A4  (VN)

// ---------- I2C: MPU6050 ----------
const uint8_t I2C_SDA = 21;    // J6
const uint8_t I2C_SCL = 22;    // J3

// ---------- Ultrasonicos ----------
const uint8_t TRIG        = 5;   // J10 (compartido)
const uint8_t ECHO_FRENTE = 19;  // J8
const uint8_t ECHO_IZQ    = 18;  // J9
const uint8_t ECHO_DER    = 23;  // J2
const uint8_t ECHO_ATRAS  = 15;  // NUEVO (propuesta): ver README, seccion de pines

// ---------- Boton de arranque ----------
const uint8_t BOTON_BOOT = 0;    // boton "BOOT" que ya trae la placa (activo en LOW)

// ============================================================================
//  PARAMETROS FISICOS  (medir / calibrar; ver README "Calibracion")
// ============================================================================
// ---------- Geometria ----------
const float DIAMETRO_RUEDA_MM = 65.0f;   // MEDIR la llanta (con el robot apoyado)
const float TROCHA_MM         = 170.0f;  // distancia entre los centros de las 2 ruedas

// ---------- Encoder ----------
// JGA25-371: 11 pulsos por vuelta del motor (por canal). Contando los 4 flancos
// de los 2 canales (cuadratura x4) y multiplicando por la reduccion de la caja
// salen las cuentas por vuelta de la rueda. La reduccion depende del modelo:
// confirmarla con la prueba 'e' (girar la rueda 10 vueltas a mano).
const float PULSOS_MOTOR       = 11.0f;
const float REDUCCION          = 34.0f;
const float CUENTAS_POR_VUELTA = PULSOS_MOTOR * 4.0f * REDUCCION;   // 1496
const float MM_POR_CUENTA      = PI * DIAMETRO_RUEDA_MM / CUENTAS_POR_VUELTA;

// ---------- Sentidos (ajustar con la prueba 'm') ----------
// Los motores van "en espejo", por eso normalmente uno de los dos se invierte.
const bool INVERTIR_MOTOR_DER = false;
const bool INVERTIR_MOTOR_IZQ = true;
const bool INVERTIR_ENC_DER   = false;
const bool INVERTIR_ENC_IZQ   = true;
// +1 si el MPU6050 esta con los componentes hacia arriba, -1 si esta de cabeza
// (prueba 'g': girar el robot a la izquierda debe dar rumbo positivo).
const float GIRO_SIGNO = 1.0f;

// ---------- Pista ----------
const float TAM_CELDA_MM      = 300.0f;  // celdas de 30 x 30 cm
const float CENTRO_A_PARED_MM = 150.0f;  // centro de celda -> cara de la pared
                                         // (restar medio grosor si el muro es grueso)

// ---------- Ultrasonicos ----------
const float US_OFFSET_FRENTE_MM = 95.0f;  // centro del robot -> cara del sensor frontal
const float US_OFFSET_ATRAS_MM  = 95.0f;  // centro del robot -> cara del sensor trasero
const float US_OFFSET_LADO_MM   = 95.0f;
// Velocidad del sonido: v = 331.3 + 0.606*T [m/s]  (a 25 C: 346.5 m/s)
const float TEMPERATURA_C    = 25.0f;
const float VEL_SONIDO_MM_US = (331.3f + 0.606f * TEMPERATURA_C) / 1000.0f;
const float US_MIN_MM        = 25.0f;    // por debajo el HC-SR04 no es confiable
const float US_MAX_MM        = 2000.0f;
const uint32_t US_PERIODO_MS = 50;       // tiempo entre disparos (ecos de 2 m tardan 12 ms)

// ============================================================================
//  PARAMETROS DE MOVIMIENTO Y CONTROL
// ============================================================================
const float VEL_CRUCERO = 300.0f;   // mm/s
const float ACELERACION = 1000.0f;  // mm/s^2 (~0.1 g, sin derrapar las llantas)
const float MARGEN_FRENADO = 0.8f;  // planear el frenado al 80% de ACELERACION: deja margen
                                    // si el ultrasonico acerca el objetivo a medio frenado

// Feedforward: el PWM que "deberia" bastar para ir a cierta velocidad.
// Valores teoricos para 200 RPM, rueda de 65 mm y bateria a ~11.4 V;
// medir los reales con la prueba 'k'.
const float KV_FF = 0.40f;    // PWM por mm/s
const float KA_FF = 0.012f;   // PWM por mm/s^2
const float KS_FF = 20.0f;    // PWM para vencer la friccion

// PD de avance (error de posicion contra el perfil)
const float KP_AVANCE = 6.0f;   // PWM por mm
const float KD_AVANCE = 0.10f;  // PWM por mm/s
// PID de rumbo (error de angulo contra el rumbo objetivo, con el giroscopio).
// La I compensa que los dos motores nunca son identicos (sin ella queda un
// error fijo de ~0.5 grados que desvia al robot unos mm por celda).
const float KP_RUMBO = 6.0f;    // PWM por grado
const float KI_RUMBO = 15.0f;   // PWM por grado*s
const float KD_RUMBO = 0.15f;   // PWM por grado/s
const float I_RUMBO_MAX = 40.0f;

// Fusion con ultrasonicos
const float US_RANGO_FUSION_MM   = 800.0f;  // ignorar paredes mas lejanas que esto
const float US_ALFA              = 0.3f;    // peso de cada lectura nueva (filtro)
const float US_COMPUERTA_MM      = 40.0f;   // lectura que difiere mas = rebote raro
const float MAX_CORRECCION_US_MM = 75.0f;   // maximo que el US puede mover el objetivo
const float US_CONGELAR_MM       = 20.0f;   // a menos de esto del final ya no se mueve el objetivo
const float DIST_SEGURA_MM       = 125.0f;  // centro del robot nunca mas cerca de una pared
const float TOL_VERIFICACION_MM  = 4.0f;    // error final que dispara la correccion

// Terminacion y seguridad
const uint32_t PERIODO_US       = 5000;   // lazo de control a 200 Hz
const float TOL_POS_MM          = 1.5f;
const float ERROR_MAX_POS_MM    = 40.0f;  // el robot no sigue al perfil -> abortar
const float ERROR_MAX_RUMBO_GR  = 20.0f;
const uint32_t ASENTAMIENTO_MAX_MS = 600;

// PWM de los motores
const uint32_t PWM_FREQ = 20000;          // 20 kHz: fuera del rango audible
const uint8_t PWM_BITS  = 8;
const int PWM_MAX       = (1 << PWM_BITS) - 1;

// ============================================================================
//  MOTORES (TB6612)
// ============================================================================
// Tabla del TB6612:  IN1=H IN2=L -> adelante | IN1=L IN2=H -> atras
//                    IN1=H IN2=H -> freno corto | STBY=L -> todo apagado
void motorEscribir(uint8_t pinPwm, uint8_t in1, uint8_t in2, float u, bool invertir) {
  if (invertir) u = -u;
  int pwm = (int)lroundf(fabsf(u));
  if (pwm > PWM_MAX) pwm = PWM_MAX;
  if (pwm == 0) {
    digitalWrite(in1, HIGH);
    digitalWrite(in2, HIGH);
  } else if (u > 0) {
    digitalWrite(in1, HIGH);
    digitalWrite(in2, LOW);
  } else {
    digitalWrite(in1, LOW);
    digitalWrite(in2, HIGH);
  }
  ledcWrite(pinPwm, pwm);
}

// u > 0 = hacia adelante, en unidades de PWM (-255 .. 255)
void motores(float uIzq, float uDer) {
  motorEscribir(PWMA, AIN1, AIN2, uDer, INVERTIR_MOTOR_DER);
  motorEscribir(PWMB, BIN1, BIN2, uIzq, INVERTIR_MOTOR_IZQ);
}

void frenar() { motores(0, 0); }

void habilitarMotores(bool encender) {
  frenar();
  digitalWrite(STBY, encender ? HIGH : LOW);
}

// ============================================================================
//  ENCODERS con el PCNT (contador de pulsos por hardware del ESP32)
// ============================================================================
// El PCNT decodifica la cuadratura sin interrupciones ni carga para el CPU y
// su filtro de glitches ignora los pulsos falsos de ~80 ns que el ESP32 puede
// generar en GPIO36/39 (errata del chip).
pcnt_unit_handle_t pcntDer = nullptr;
pcnt_unit_handle_t pcntIzq = nullptr;

#define REVISAR(x)                                                       \
  do {                                                                   \
    esp_err_t e_ = (x);                                                  \
    if (e_ != ESP_OK) {                                                  \
      Serial.printf("  ERROR %s en %s\n", esp_err_to_name(e_), #x);      \
      return false;                                                      \
    }                                                                    \
  } while (0)

bool configurarEncoder(uint8_t pinC1, uint8_t pinC2, pcnt_unit_handle_t* unidad) {
  pcnt_unit_config_t cfg = {};
  cfg.low_limit = -30000;
  cfg.high_limit = 30000;
  cfg.flags.accum_count = 1;   // acumula al desbordar: cuenta de 32 bits
  REVISAR(pcnt_new_unit(&cfg, unidad));

  pcnt_glitch_filter_config_t filtro = {};
  filtro.max_glitch_ns = 1000;
  REVISAR(pcnt_unit_set_glitch_filter(*unidad, &filtro));

  // Canal 1: cuenta flancos de C1 mirando el nivel de C2, y viceversa (x4)
  pcnt_chan_config_t cfgC1 = {};
  cfgC1.edge_gpio_num = pinC1;
  cfgC1.level_gpio_num = pinC2;
  pcnt_chan_config_t cfgC2 = {};
  cfgC2.edge_gpio_num = pinC2;
  cfgC2.level_gpio_num = pinC1;
  pcnt_channel_handle_t canal1 = nullptr;
  pcnt_channel_handle_t canal2 = nullptr;
  REVISAR(pcnt_new_channel(*unidad, &cfgC1, &canal1));
  REVISAR(pcnt_new_channel(*unidad, &cfgC2, &canal2));
  REVISAR(pcnt_channel_set_edge_action(canal1, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                       PCNT_CHANNEL_EDGE_ACTION_INCREASE));
  REVISAR(pcnt_channel_set_level_action(canal1, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                        PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
  REVISAR(pcnt_channel_set_edge_action(canal2, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                       PCNT_CHANNEL_EDGE_ACTION_DECREASE));
  REVISAR(pcnt_channel_set_level_action(canal2, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                        PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

  REVISAR(pcnt_unit_add_watch_point(*unidad, cfg.low_limit));
  REVISAR(pcnt_unit_add_watch_point(*unidad, cfg.high_limit));
  REVISAR(pcnt_unit_enable(*unidad));
  REVISAR(pcnt_unit_clear_count(*unidad));
  REVISAR(pcnt_unit_start(*unidad));
  return true;
}

int32_t cuentas(pcnt_unit_handle_t unidad) {
  int c = 0;
  if (unidad) pcnt_unit_get_count(unidad, &c);
  return c;
}

int32_t cuentasDer() { return INVERTIR_ENC_DER ? -cuentas(pcntDer) : cuentas(pcntDer); }
int32_t cuentasIzq() { return INVERTIR_ENC_IZQ ? -cuentas(pcntIzq) : cuentas(pcntIzq); }
float posDerMM() { return cuentasDer() * MM_POR_CUENTA; }
float posIzqMM() { return cuentasIzq() * MM_POR_CUENTA; }

// ============================================================================
//  GIROSCOPIO MPU6050 (solo el eje Z: el robot se mueve en un plano)
// ============================================================================
const uint8_t MPU_DIR = 0x68;           // 0x69 si AD0 esta en HIGH
const float MPU_LSB_POR_GPS = 65.5f;    // escala +-500 grados/s

bool hayGiro = false;      // si no hay MPU, el rumbo se estima con los encoders
float biasGiro = 0;        // lectura cruda con el robot quieto
float rumbo = 0;           // grados; positivo = giro a la izquierda (antihorario)
float velGiro = 0;         // grados/s
float rumboObjetivo = 0;   // rumbo que se debe mantener al avanzar
uint32_t tGiroUs = 0;
float rumboEncAnterior = 0;

bool mpuEscribir(uint8_t reg, uint8_t valor) {
  Wire.beginTransmission(MPU_DIR);
  Wire.write(reg);
  Wire.write(valor);
  return Wire.endTransmission() == 0;
}

bool mpuLeer(uint8_t reg, uint8_t* datos, uint8_t n) {
  Wire.beginTransmission(MPU_DIR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_DIR, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) datos[i] = Wire.read();
  return true;
}

bool mpuLeerGiroZ(int16_t& crudo) {
  uint8_t b[2];
  if (!mpuLeer(0x47, b, 2)) return false;   // GYRO_ZOUT_H, GYRO_ZOUT_L
  crudo = (int16_t)((b[0] << 8) | b[1]);
  return true;
}

bool iniciarMPU() {
  if (!mpuEscribir(0x6B, 0x01)) return false;  // PWR_MGMT_1: despertar, reloj del giro X
  delay(50);
  uint8_t quien = 0;
  mpuLeer(0x75, &quien, 1);                     // WHO_AM_I (0x68 original; clones 0x70/0x72...)
  Serial.printf("  MPU6050 encontrado (WHO_AM_I = 0x%02X)\n", quien);
  mpuEscribir(0x1A, 0x03);   // CONFIG: filtro pasa-bajas de 42 Hz
  mpuEscribir(0x19, 0x00);   // SMPLRT_DIV: 1 kHz
  mpuEscribir(0x1B, 0x08);   // GYRO_CONFIG: +-500 grados/s
  delay(50);
  return true;
}

// Promedia la lectura en reposo (bias). El robot NO debe moverse.
bool calibrarGiro() {
  const int N = 800;
  for (int intento = 0; intento < 3; intento++) {
    double suma = 0, suma2 = 0;
    int ok = 0;
    for (int i = 0; i < N; i++) {
      int16_t r;
      if (mpuLeerGiroZ(r)) {
        suma += r;
        suma2 += (double)r * r;
        ok++;
      }
      delay(2);
    }
    if (ok < N / 2) return false;
    double media = suma / ok;
    double desv = sqrt(fmax(0.0, suma2 / ok - media * media));
    if (desv < 0.5 * MPU_LSB_POR_GPS) {          // ruido < 0.5 grados/s: estaba quieto
      biasGiro = media;
      Serial.printf("  Bias del giroscopio: %.2f grados/s (ruido %.3f)\n",
                    media / MPU_LSB_POR_GPS, desv / MPU_LSB_POR_GPS);
      return true;
    }
    Serial.println("  El robot se movio durante la calibracion, repitiendo...");
  }
  return false;
}

float rumboEncoders() { return (posDerMM() - posIzqMM()) / TROCHA_MM * (180.0f / PI); }

// Integra la velocidad angular. Llamar seguido (cada ~5 ms).
// quieto = los encoders no se han movido: se aprovecha para re-estimar el bias
// (el bias del MPU6050 cambia con la temperatura).
void actualizarRumbo(bool quieto) {
  uint32_t ahora = micros();
  float dt = (ahora - tGiroUs) * 1e-6f;
  tGiroUs = ahora;
  if (hayGiro) {
    int16_t crudo;
    if (mpuLeerGiroZ(crudo)) {   // si la lectura falla se usa la velocidad anterior
      float r = crudo - biasGiro;
      if (quieto && fabsf(r) < 2.0f * MPU_LSB_POR_GPS) {
        biasGiro += 0.002f * r;
        velGiro = 0;
        return;
      }
      velGiro = GIRO_SIGNO * r / MPU_LSB_POR_GPS;
    }
    rumbo += velGiro * dt;
  } else {
    float r = rumboEncoders();
    velGiro = dt > 0 ? (r - rumboEncAnterior) / dt : 0;
    rumbo += r - rumboEncAnterior;
    rumboEncAnterior = r;
  }
}

float envolver180(float a) {
  while (a > 180.0f) a -= 360.0f;
  while (a < -180.0f) a += 360.0f;
  return a;
}

// ============================================================================
//  ULTRASONICOS (no bloqueantes: el eco se mide con interrupciones)
// ============================================================================
// pulseIn() detendria el lazo de control hasta 12 ms por sensor. Aqui el TRIG
// compartido dispara todos a la vez y cada ECHO guarda su tiempo en una ISR.
struct Ultrasonico {
  const char* nombre;
  uint8_t pinEcho;
  float offsetMM;                 // centro del robot -> cara del sensor
  volatile uint32_t tSubida;
  volatile uint32_t anchoUs;
  volatile bool listo;
};

enum { US_FRENTE = 0, US_ATRAS, US_IZQ, US_DER, US_NUM };
Ultrasonico ultrasonicos[US_NUM] = {
  {"frente", ECHO_FRENTE, US_OFFSET_FRENTE_MM, 0, 0, false},
  {"atras",  ECHO_ATRAS,  US_OFFSET_ATRAS_MM,  0, 0, false},
  {"izq",    ECHO_IZQ,    US_OFFSET_LADO_MM,   0, 0, false},
  {"der",    ECHO_DER,    US_OFFSET_LADO_MM,   0, 0, false},
};
uint32_t tDisparoMs = 0;

void IRAM_ATTR isrEco(void* arg) {
  Ultrasonico* us = (Ultrasonico*)arg;
  uint32_t t = micros();
  if (digitalRead(us->pinEcho)) {
    us->tSubida = t;
  } else if (us->tSubida != 0) {
    us->anchoUs = t - us->tSubida;
    us->tSubida = 0;
    us->listo = true;
  }
}

void dispararUltrasonicos() {
  for (int i = 0; i < US_NUM; i++) {
    ultrasonicos[i].listo = false;
    ultrasonicos[i].tSubida = 0;
  }
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  tDisparoMs = millis();
}

// Distancia del CENTRO del robot a la pared, en mm (NAN si no hubo eco valido)
float distanciaCentro(int i) {
  const Ultrasonico& us = ultrasonicos[i];
  if (!us.listo) return NAN;
  float d = us.anchoUs * VEL_SONIDO_MM_US * 0.5f;   // ida y vuelta -> /2
  if (d < US_MIN_MM || d > US_MAX_MM) return NAN;
  return d + us.offsetMM;
}

void ordenar(float* v, int n) {
  for (int i = 1; i < n; i++) {
    float x = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
    v[j + 1] = x;
  }
}

void servicioFondo();

void esperarMs(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) servicioFondo();
}

// Con el robot quieto: mediana de n disparos para cada sensor
void medirParedes(float salida[US_NUM], int n) {
  float lecturas[US_NUM][9];
  int k[US_NUM] = {0};
  if (n > 9) n = 9;
  for (int j = 0; j < n; j++) {
    dispararUltrasonicos();
    esperarMs(US_PERIODO_MS);
    for (int i = 0; i < US_NUM; i++) {
      float c = distanciaCentro(i);
      if (!isnan(c)) lecturas[i][k[i]++] = c;
    }
  }
  for (int i = 0; i < US_NUM; i++) {
    if (k[i] * 2 <= n) {           // la mayoria sin eco -> no hay pared confiable
      salida[i] = NAN;
    } else {
      ordenar(lecturas[i], k[i]);
      salida[i] = lecturas[i][k[i] / 2];
    }
  }
}

// ============================================================================
//  FUSION: posicion de la pared en el eje de los encoders
// ============================================================================
// Una pared no se mueve. Si s es lo que marcan los encoders y c la distancia
// centro->pared, entonces  posEnc = s + dir*c  debe ser CONSTANTE. Cada lectura
// refina esa constante (filtro + rechazo de rebotes) y con ella se calcula
// donde frenar:  objetivo = posEnc - dir*objetivoCentro.
// Ventaja: no importa el retraso del ultrasonico, porque cada lectura se
// asocia a la posicion de encoders del momento en que se disparo.
struct Pared {
  float dir;              // +1 = pared de enfrente, -1 = pared de atras
  bool activa;
  int muestras;
  int rechazosSeguidos;
  float posEnc;           // mm
  float objetivoCentro;   // distancia centro->pared deseada al terminar
};

float ajustarARejilla(float c) {
  float k = roundf((c - CENTRO_A_PARED_MM) / TAM_CELDA_MM);
  if (k < 0) k = 0;
  return CENTRO_A_PARED_MM + k * TAM_CELDA_MM;
}

void paredIniciar(Pared& p, float dir) {
  p.dir = dir;
  p.activa = false;
  p.muestras = 0;
  p.rechazosSeguidos = 0;
  p.posEnc = 0;
  p.objetivoCentro = 0;
}

void paredIncorporar(Pared& p, float c, float sMedicion, float objetivoEnc, bool rejilla,
                     int peso) {
  if (isnan(c) || c > US_RANGO_FUSION_MM) return;
  float w = sMedicion + p.dir * c;
  if (p.activa && fabsf(w - p.posEnc) > US_COMPUERTA_MM) {
    if (++p.rechazosSeguidos < 3) return;   // rebote raro: se ignora
    p.activa = false;                        // 3 seguidas: la estimacion era la mala
  }
  p.rechazosSeguidos = 0;
  if (!p.activa) {
    p.activa = true;
    p.muestras = 0;
    p.posEnc = w;
    // A que distancia de esta pared quedariamos si los encoders fueran perfectos
    float cFinal = p.dir * (w - objetivoEnc);
    p.objetivoCentro = rejilla ? ajustarARejilla(cFinal) : cFinal;
  } else {
    p.posEnc += US_ALFA * (w - p.posEnc);
  }
  p.muestras += peso;
}

bool paredConfiable(const Pared& p) { return p.activa && p.muestras >= 2; }

float objetivoDePared(const Pared& p) { return p.posEnc - p.dir * p.objetivoCentro; }

// Combina encoders + pared de enfrente + pared de atras (la mas cercana pesa mas)
float calcularObjetivo(float objetivoEnc, const Pared& frente, const Pared& atras) {
  const Pared* paredes[2] = {&frente, &atras};
  float suma = 0, pesos = 0;
  for (const Pared* p : paredes) {
    if (!paredConfiable(*p)) continue;
    float t = objetivoDePared(*p);
    if (fabsf(t - objetivoEnc) > MAX_CORRECCION_US_MM) continue;   // no cuadra: ignorar
    float w = 1.0f / sq(fabsf(p->objetivoCentro) + 50.0f);
    suma += w * t;
    pesos += w;
  }
  float objetivo = pesos > 0 ? suma / pesos : objetivoEnc;
  // Seguridad: nunca acercarse a una pared mas de DIST_SEGURA_MM
  if (paredConfiable(frente)) objetivo = fminf(objetivo, frente.posEnc - DIST_SEGURA_MM);
  if (paredConfiable(atras))  objetivo = fmaxf(objetivo, atras.posEnc + DIST_SEGURA_MM);
  return objetivo;
}

// ============================================================================
//  REGISTRO (para afinar ganancias: copiar el CSV a una hoja de calculo)
// ============================================================================
struct Muestra {
  float t, ref, pos, vRef, vel, rumbo, uIzq, uDer, objetivo;
};
const int REG_MAX = 600;
Muestra registro[REG_MAX];
int regN = 0;
bool registroActivo = false;

void imprimirRegistro() {
  Serial.println("t_s,ref_mm,pos_mm,vref_mms,vel_mms,rumbo_gr,u_izq,u_der,objetivo_mm");
  for (int i = 0; i < regN; i++) {
    const Muestra& m = registro[i];
    Serial.printf("%.3f,%.1f,%.1f,%.0f,%.0f,%.2f,%.0f,%.0f,%.1f\n", m.t, m.ref, m.pos,
                  m.vRef, m.vel, m.rumbo, m.uIzq, m.uDer, m.objetivo);
  }
}

// ============================================================================
//  MOVIMIENTO RECTO
// ============================================================================
enum Resultado { LLEGO, ABORTADO, ERROR_SEGUIMIENTO, ERROR_RUMBO, TIEMPO_AGOTADO };

const char* textoResultado(Resultado r) {
  switch (r) {
    case LLEGO: return "llego";
    case ABORTADO: return "detenido por el usuario";
    case ERROR_SEGUIMIENTO: return "ERROR: las ruedas no siguen al perfil (atorado o sentido invertido)";
    case ERROR_RUMBO: return "ERROR: el rumbo se desvio demasiado (signo del giro o una rueda trabada)";
    case TIEMPO_AGOTADO: return "ERROR: tiempo agotado";
  }
  return "?";
}

bool botonPresionado() { return digitalRead(BOTON_BOOT) == LOW; }

// Lazo de control. Mueve el robot 'distanciaMM' (negativo = reversa)
// manteniendo rumboObjetivo. Si se le pasan paredes, las usa para ajustar
// el punto de frenado mientras avanza.
Resultado moverRecto(float distanciaMM, Pared* frente, Pared* atras, bool rejilla,
                     float& objetivoFinal, float& errorRumboMax) {
  const float s0 = 0.5f * (posDerMM() + posIzqMM());
  const float objetivoEnc = s0 + distanciaMM;
  const float tMaxS = fabsf(distanciaMM) / VEL_CRUCERO + VEL_CRUCERO / ACELERACION;
  const uint32_t limiteMs = (uint32_t)(1500.0f * tMaxS) + 1000;

  float posRef = s0, vRef = 0, aRef = 0;
  float sAnt = s0, vel = 0, errPosAnt = 0, iRumbo = 0;
  float objetivo = objetivoEnc;
  float sDisparo = s0;
  errorRumboMax = 0;
  uint32_t tAsentado = 0, tLlegada = 0;
  int ciclo = 0;

  while (Serial.available()) Serial.read();
  dispararUltrasonicos();
  const uint32_t tInicio = millis();
  uint32_t tAnt = micros();
  uint32_t tSiguiente = tAnt + PERIODO_US;
  Resultado res = TIEMPO_AGOTADO;

  while (true) {
    while ((int32_t)(micros() - tSiguiente) < 0) {}   // esperar el siguiente ciclo
    uint32_t ahora = micros();
    if ((int32_t)(ahora - tSiguiente) > (int32_t)PERIODO_US) tSiguiente = ahora;
    tSiguiente += PERIODO_US;
    float dt = (ahora - tAnt) * 1e-6f;
    tAnt = ahora;

    // ---- 1. Sensores ----
    const float s = 0.5f * (posDerMM() + posIzqMM());   // avance del centro
    vel += 0.3f * ((s - sAnt) / dt - vel);
    sAnt = s;
    actualizarRumbo(false);

    if (millis() - tDisparoMs >= US_PERIODO_MS) {
      // Lecturas del disparo anterior, asociadas a la posicion de ese disparo
      if (frente) paredIncorporar(*frente, distanciaCentro(US_FRENTE), sDisparo, objetivoEnc, rejilla, 1);
      if (atras)  paredIncorporar(*atras,  distanciaCentro(US_ATRAS),  sDisparo, objetivoEnc, rejilla, 1);
      sDisparo = s;
      dispararUltrasonicos();
    }
    if (frente && atras && fabsf(objetivo - s) > US_CONGELAR_MM)
      objetivo = calcularObjetivo(objetivoEnc, *frente, *atras);

    // ---- 2. Perfil trapezoidal "en linea" ----
    // Acelera hasta VEL_CRUCERO y frena justo a tiempo: la velocidad nunca pasa
    // de sqrt(2*a*d) (fisica: v^2 = 2*a*d), asi puede cambiar el objetivo a medio camino.
    float restante = objetivo - posRef;
    float vFrenado = sqrtf(2.0f * MARGEN_FRENADO * ACELERACION * fabsf(restante));
    float vDeseada = copysignf(fminf(VEL_CRUCERO, vFrenado), restante);
    float dv = constrain(vDeseada - vRef, -ACELERACION * dt, ACELERACION * dt);
    aRef = dv / dt;
    vRef += dv;
    posRef += vRef * dt;
    bool perfilTerminado = fabsf(objetivo - posRef) < 0.5f && fabsf(vRef) < 15.0f;
    if (perfilTerminado) {
      posRef = objetivo;
      vRef = 0;
      aRef = 0;
    }

    // ---- 3. Control de avance: feedforward + PD sobre el error de posicion ----
    float errPos = posRef - s;
    float uAvance = KP_AVANCE * errPos + KD_AVANCE * (errPos - errPosAnt) / dt +
                    KV_FF * vRef + KA_FF * aRef;
    errPosAnt = errPos;
    if (fabsf(vRef) > 0.5f) {
      uAvance += KS_FF * constrain(vRef / 20.0f, -1.0f, 1.0f);   // friccion, suave cerca de 0
    } else if (fabsf(errPos) > TOL_POS_MM) {
      uAvance += copysignf(KS_FF, errPos);                       // empujon final
    }

    // ---- 4. Control de rumbo con el giroscopio (PID) ----
    float errRumbo = envolver180(rumboObjetivo - rumbo);
    errorRumboMax = fmaxf(errorRumboMax, fabsf(errRumbo));
    iRumbo = constrain(iRumbo + KI_RUMBO * errRumbo * dt, -I_RUMBO_MAX, I_RUMBO_MAX);
    float uRumbo = KP_RUMBO * errRumbo + iRumbo - KD_RUMBO * velGiro;
    uRumbo = constrain(uRumbo, -0.5f * PWM_MAX, 0.5f * PWM_MAX);

    // ---- 5. Mezcla: si se satura, se sacrifica avance y no correccion de rumbo ----
    float limite = PWM_MAX - fabsf(uRumbo);
    uAvance = constrain(uAvance, -limite, limite);
    // uRumbo > 0: el robot se desvio a la derecha -> rueda derecha mas rapida -> corrige a la izquierda
    float uDer = uAvance + uRumbo;
    float uIzq = uAvance - uRumbo;
    motores(uIzq, uDer);

    if (registroActivo && (ciclo++ & 1) == 0 && regN < REG_MAX) {
      registro[regN++] = {(millis() - tInicio) * 1e-3f, posRef - s0, s - s0, vRef, vel,
                          rumbo, uIzq, uDer, objetivo - s0};
    }

    // ---- 6. Terminar ----
    if (Serial.available() || botonPresionado()) { res = ABORTADO; break; }
    if (fabsf(errPos) > ERROR_MAX_POS_MM) { res = ERROR_SEGUIMIENTO; break; }
    if (fabsf(errRumbo) > ERROR_MAX_RUMBO_GR) { res = ERROR_RUMBO; break; }
    if (millis() - tInicio > limiteMs) { res = TIEMPO_AGOTADO; break; }
    if (perfilTerminado) {
      if (tLlegada == 0) tLlegada = millis();
      bool quieto = fabsf(objetivo - s) <= TOL_POS_MM && fabsf(vel) < 5.0f;
      if (!quieto) tAsentado = millis();
      if (millis() - tAsentado > 60 || millis() - tLlegada > ASENTAMIENTO_MAX_MS) {
        res = LLEGO;
        break;
      }
    } else {
      tLlegada = 0;
      tAsentado = millis();
    }
  }
  frenar();
  objetivoFinal = objetivo - s0;
  while (Serial.available()) Serial.read();
  while (botonPresionado()) servicioFondo();
  return res;
}

// Error de posicion segun los ultrasonicos (+ = falta avanzar). NAN si no hay pared.
// Nunca pide acercarse a una pared mas de DIST_SEGURA_MM.
float errorSegunParedes(const float c[US_NUM], const Pared& frente, const Pared& atras) {
  float suma = 0, pesos = 0;
  if (frente.activa && !isnan(c[US_FRENTE])) {
    float e = c[US_FRENTE] - fmaxf(frente.objetivoCentro, DIST_SEGURA_MM);
    float w = 1.0f / sq(c[US_FRENTE] + 50.0f);
    suma += w * e;
    pesos += w;
  }
  if (atras.activa && !isnan(c[US_ATRAS])) {
    float e = fmaxf(atras.objetivoCentro, DIST_SEGURA_MM) - c[US_ATRAS];
    float w = 1.0f / sq(c[US_ATRAS] + 50.0f);
    suma += w * e;
    pesos += w;
  }
  return pesos > 0 ? suma / pesos : NAN;
}

// Avance completo: medir paredes -> mover con fusion -> verificar -> corregir
Resultado avanzarRecto(float distanciaMM, bool rejilla) {
  Serial.printf("\n>> Avanzar %.0f mm%s\n", distanciaMM, rejilla ? " (centrar en la celda)" : "");
  habilitarMotores(true);
  regN = 0;

  float antes[US_NUM];
  medirParedes(antes, 5);
  Serial.printf("   Paredes antes: frente %.0f mm, atras %.0f mm (desde el centro)\n",
                antes[US_FRENTE], antes[US_ATRAS]);

  // En modo celdas: no intentar atravesar una pared
  if (rejilla) {
    float cPared = distanciaMM >= 0 ? antes[US_FRENTE] : antes[US_ATRAS];
    if (!isnan(cPared) && cPared <= US_RANGO_FUSION_MM) {
      float celdasLibres = floorf((cPared - CENTRO_A_PARED_MM + MAX_CORRECCION_US_MM) / TAM_CELDA_MM);
      float maxMM = fmaxf(0.0f, celdasLibres) * TAM_CELDA_MM;
      if (fabsf(distanciaMM) > maxMM) {
        Serial.printf("   Pared a %.0f mm: solo caben %.0f celda(s)\n", cPared, maxMM / TAM_CELDA_MM);
        distanciaMM = copysignf(maxMM, distanciaMM);
      }
    }
  }

  const float s0 = 0.5f * (posDerMM() + posIzqMM());
  Pared frente, atras;
  paredIniciar(frente, +1.0f);
  paredIniciar(atras, -1.0f);
  paredIncorporar(frente, antes[US_FRENTE], s0, s0 + distanciaMM, rejilla, 3);
  paredIncorporar(atras,  antes[US_ATRAS],  s0, s0 + distanciaMM, rejilla, 3);

  uint32_t t0 = millis();
  float objetivoUsado = distanciaMM, errorRumboMax = 0;
  Resultado res = LLEGO;
  if (fabsf(distanciaMM) > 0.5f) {
    res = moverRecto(distanciaMM, &frente, &atras, rejilla, objetivoUsado, errorRumboMax);
  }
  float duracion = (millis() - t0) * 1e-3f;
  esperarMs(250);   // que se asiente antes de medir

  // Verificacion con el robot quieto y, si hace falta, una correccion pequena
  float despues[US_NUM];
  medirParedes(despues, 5);
  float errUS = errorSegunParedes(despues, frente, atras);
  float corregido = 0;
  if (res == LLEGO && !isnan(errUS) && fabsf(errUS) > TOL_VERIFICACION_MM &&
      fabsf(errUS) <= MAX_CORRECCION_US_MM) {
    float objCorr, errRumboCorr;
    Serial.printf("   Verificacion: faltan %.1f mm segun ultrasonicos -> corrigiendo\n", errUS);
    res = moverRecto(errUS, nullptr, nullptr, false, objCorr, errRumboCorr);
    corregido = errUS;
    esperarMs(250);
    medirParedes(despues, 5);
    errUS = errorSegunParedes(despues, frente, atras);
  }
  habilitarMotores(false);

  float sFinal = 0.5f * (posDerMM() + posIzqMM());
  Serial.println("   ---------------- Resultado ----------------");
  Serial.printf("   Estado: %s  (%.2f s)\n", textoResultado(res), duracion);
  Serial.printf("   Pedido %.1f mm | objetivo usado %.1f mm (ajuste US %+.1f mm)\n",
                distanciaMM, objetivoUsado, objetivoUsado - distanciaMM);
  if (corregido != 0) Serial.printf("   Correccion final: %+.1f mm\n", corregido);
  Serial.printf("   Encoders: avanzo %.1f mm\n", sFinal - s0);
  Serial.printf("   Rumbo: objetivo %.2f, final %.2f grados (desvio max %.2f)\n", rumboObjetivo,
                rumbo, errorRumboMax);
  if (!isnan(antes[US_FRENTE]) && !isnan(despues[US_FRENTE]))
    Serial.printf("   US frente: %.0f -> %.0f mm  => avanzo %.1f mm\n", antes[US_FRENTE],
                  despues[US_FRENTE], antes[US_FRENTE] - despues[US_FRENTE]);
  if (!isnan(antes[US_ATRAS]) && !isnan(despues[US_ATRAS]))
    Serial.printf("   US atras:  %.0f -> %.0f mm  => avanzo %.1f mm\n", antes[US_ATRAS],
                  despues[US_ATRAS], despues[US_ATRAS] - antes[US_ATRAS]);
  if (!isnan(errUS)) Serial.printf("   Error final segun ultrasonicos: %+.1f mm\n", errUS);
  else Serial.println("   Sin paredes en rango: solo encoders");
  if (registroActivo) imprimirRegistro();
  return res;
}

Resultado avanzarCeldas(int n) { return avanzarRecto(n * TAM_CELDA_MM, true); }

// ============================================================================
//  PRUEBAS DE DIAGNOSTICO
// ============================================================================
bool teclaParaSalir() {
  if (!Serial.available()) return false;
  while (Serial.available()) Serial.read();
  return true;
}

// 'm': cada motor por separado, CON EL ROBOT LEVANTADO
void pruebaMotores() {
  Serial.println("\n>> Prueba de motores (robot LEVANTADO). Cada rueda debe girar HACIA ADELANTE.");
  habilitarMotores(true);
  for (int lado = 0; lado < 2; lado++) {
    const char* nombre = lado == 0 ? "DERECHO" : "IZQUIERDO";
    int32_t d0 = cuentasDer(), i0 = cuentasIzq();
    if (lado == 0) motores(0, 90); else motores(90, 0);
    esperarMs(700);
    frenar();
    esperarMs(300);
    int32_t dPropio = lado == 0 ? cuentasDer() - d0 : cuentasIzq() - i0;
    int32_t dOtro = lado == 0 ? cuentasIzq() - i0 : cuentasDer() - d0;
    Serial.printf("   Motor %s: encoder propio %+ld, encoder del otro lado %+ld\n", nombre,
                  (long)dPropio, (long)dOtro);
    if (abs(dOtro) > abs(dPropio) && abs(dOtro) > 20)
      Serial.println("     -> Se movio el encoder del OTRO lado: cables de encoder cruzados (A<->B)");
    else if (abs(dPropio) < 20)
      Serial.println("     -> Casi sin cuentas: revisar alimentacion/cables del encoder, STBY y bateria");
    else if (dPropio < 0)
      Serial.printf("     -> Cuenta negativa. Si la rueda giro hacia ADELANTE invierte INVERTIR_ENC_%s;\n"
                    "        si giro hacia ATRAS invierte INVERTIR_MOTOR_%s\n",
                    lado == 0 ? "DER" : "IZQ", lado == 0 ? "DER" : "IZQ");
    else
      Serial.printf("     -> OK si la rueda giro hacia ADELANTE (si giro hacia atras, invierte\n"
                    "        INVERTIR_MOTOR_%s Y INVERTIR_ENC_%s)\n",
                    lado == 0 ? "DER" : "IZQ", lado == 0 ? "DER" : "IZQ");
  }
  habilitarMotores(false);
}

// 'e': girar las ruedas a mano para ver/calibrar las cuentas
void pruebaEncoders() {
  Serial.println("\n>> Encoders: gira cada rueda 10 vueltas HACIA ADELANTE (tecla = salir)");
  Serial.printf("   Esperado: +%.0f cuentas por vuelta, %.1f mm por vuelta\n", CUENTAS_POR_VUELTA,
                PI * DIAMETRO_RUEDA_MM);
  int32_t d0 = cuentasDer(), i0 = cuentasIzq();
  uint32_t t = 0;
  while (!teclaParaSalir()) {
    servicioFondo();
    if (millis() - t < 250) continue;
    t = millis();
    int32_t d = cuentasDer() - d0, i = cuentasIzq() - i0;
    Serial.printf("   der %+7ld (%6.2f vueltas)   izq %+7ld (%6.2f vueltas)\n", (long)d,
                  d / CUENTAS_POR_VUELTA, (long)i, i / CUENTAS_POR_VUELTA);
  }
}

// 'g': ver el rumbo; girar el robot a la izquierda debe dar valores positivos
void pruebaGiro() {
  Serial.printf("\n>> Giroscopio (%s). Gira el robot 90 grados a la IZQUIERDA: debe marcar +90."
                " (tecla = salir)\n", hayGiro ? "MPU6050" : "SIN MPU: usando encoders");
  uint32_t t = 0;
  while (!teclaParaSalir()) {
    servicioFondo();
    if (millis() - t < 200) continue;
    t = millis();
    Serial.printf("   rumbo %8.2f grados   vel %7.2f grados/s\n", rumbo, velGiro);
  }
}

// 'u': lecturas de los 4 ultrasonicos
void pruebaUltrasonicos() {
  Serial.println("\n>> Ultrasonicos: distancia del CENTRO del robot a la pared (tecla = salir)");
  while (!teclaParaSalir()) {
    float c[US_NUM];
    medirParedes(c, 1);
    Serial.printf("   frente %6.0f   atras %6.0f   izq %6.0f   der %6.0f  mm\n", c[US_FRENTE],
                  c[US_ATRAS], c[US_IZQ], c[US_DER]);
  }
}

// 'k': mide la velocidad real a varios PWM y sugiere KV_FF y KS_FF.
// Necesita ~40 cm libres al frente (va y regresa), o el robot levantado.
void pruebaCaracterizar() {
  Serial.println("\n>> Caracterizacion de motores (va y regresa a cada PWM)");
  const int pasos[] = {60, 100, 140, 180, 220};
  float sx = 0, sy = 0, sxx = 0, sxy = 0;
  int n = 0;
  habilitarMotores(true);
  for (int pwm : pasos) {
    for (int sentido = 1; sentido >= -1; sentido -= 2) {
      motores(sentido * pwm, sentido * pwm);
      esperarMs(250);                                  // acelerar
      float d0 = posDerMM(), i0 = posIzqMM();
      uint32_t t0 = micros();
      esperarMs(300);                                  // medir a velocidad constante
      float dt = (micros() - t0) * 1e-6f;
      float vD = sentido * (posDerMM() - d0) / dt;
      float vI = sentido * (posIzqMM() - i0) / dt;
      frenar();
      esperarMs(300);
      Serial.printf("   PWM %s%3d: der %6.0f mm/s   izq %6.0f mm/s\n", sentido > 0 ? "+" : "-",
                    pwm, vD, vI);
      for (float v : {vD, vI}) {
        if (v < 20) continue;
        sx += v; sy += pwm; sxx += v * v; sxy += v * pwm; n++;
      }
    }
  }
  habilitarMotores(false);
  if (n < 4) {
    Serial.println("   No hubo suficientes datos (revisar motores con 'm')");
    return;
  }
  // Recta PWM = KS + KV * v por minimos cuadrados
  float kv = (n * sxy - sx * sy) / (n * sxx - sx * sx);
  float ks = (sy - kv * sx) / n;
  Serial.printf("   Sugerido: KV_FF = %.3f   KS_FF = %.1f   (vel. maxima ~%.0f mm/s)\n", kv, ks,
                (PWM_MAX - ks) / kv);
  Serial.printf("   Recomendado VEL_CRUCERO <= %.0f mm/s (70%% del maximo)\n",
                0.7f * (PWM_MAX - ks) / kv);
}

// ============================================================================
//  INTERFAZ
// ============================================================================
void imprimirAyuda() {
  Serial.println(F(
    "\n===== Roborregos - prueba de avance recto =====\n"
    "  Boton BOOT : rumbo actual = recto y avanza 1 celda\n"
    "  a<mm>      : avanzar mm (ej. a300, a-150)\n"
    "  c<n>       : avanzar n celdas centrando con las paredes (ej. c1, c2)\n"
    "  m          : prueba de motores y sentidos (robot levantado)\n"
    "  e          : prueba de encoders (girar ruedas a mano)\n"
    "  g          : prueba del giroscopio\n"
    "  u          : prueba de ultrasonicos\n"
    "  k          : caracterizar motores (sugiere KV_FF y KS_FF)\n"
    "  z          : rumbo actual = 0 (y objetivo = 0)\n"
    "  r          : activar/desactivar registro CSV del movimiento\n"
    "  h          : esta ayuda\n"
    "  Cualquier tecla o BOOT durante un movimiento: detener"));
}

void ejecutarComando(const char* linea) {
  char cmd = linea[0];
  float valor = atof(linea + 1);
  switch (cmd) {
    case 'a': avanzarRecto(valor != 0 ? valor : TAM_CELDA_MM, false); break;
    case 'c': avanzarCeldas(valor != 0 ? (int)valor : 1); break;
    case 'm': pruebaMotores(); break;
    case 'e': pruebaEncoders(); break;
    case 'g': pruebaGiro(); break;
    case 'u': pruebaUltrasonicos(); break;
    case 'k': pruebaCaracterizar(); break;
    case 'z':
      rumbo = 0;
      rumboObjetivo = 0;
      Serial.println("Rumbo = 0");
      break;
    case 'r':
      registroActivo = !registroActivo;
      Serial.printf("Registro CSV %s\n", registroActivo ? "ACTIVADO" : "desactivado");
      break;
    default: imprimirAyuda(); break;
  }
}

// Lee comandos del monitor serie. Si no hay terminador de linea, ejecuta
// despues de 100 ms sin recibir nada.
void servicioSerie() {
  static char linea[24];
  static uint8_t n = 0;
  static uint32_t tUltimo = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    tUltimo = millis();
    if (ch == '\n' || ch == '\r') {
      if (n > 0) {
        linea[n] = 0;
        n = 0;
        ejecutarComando(linea);
      }
    } else if (ch != ' ' && n < sizeof(linea) - 1) {
      linea[n++] = tolower(ch);
    }
  }
  if (n > 0 && millis() - tUltimo > 100) {
    linea[n] = 0;
    n = 0;
    ejecutarComando(linea);
  }
}

// Tareas mientras el robot espera: integrar el giroscopio (y re-estimar su
// bias si las ruedas no se mueven).
void servicioFondo() {
  static uint32_t tUlt = 0, tMovimiento = 0;
  static int32_t dAnt = 0, iAnt = 0;
  if (micros() - tUlt < PERIODO_US) return;
  tUlt = micros();
  int32_t d = cuentasDer(), i = cuentasIzq();
  if (d != dAnt || i != iAnt) {
    dAnt = d;
    iAnt = i;
    tMovimiento = millis();
  }
  actualizarRumbo(millis() - tMovimiento > 500);
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nIniciando...");

  // Driver: direccion y STBY son salidas digitales
  pinMode(AIN1, OUTPUT);
  pinMode(AIN2, OUTPUT);
  pinMode(BIN1, OUTPUT);
  pinMode(BIN2, OUTPUT);
  pinMode(STBY, OUTPUT);
  digitalWrite(STBY, LOW);   // motores apagados hasta la condicion de arranque
  ledcAttach(PWMA, PWM_FREQ, PWM_BITS);
  ledcAttach(PWMB, PWM_FREQ, PWM_BITS);
  frenar();

  // Encoders (el PCNT configura los pines; 34-39 no tienen pull-up interno)
  bool encOk = configurarEncoder(ENC_A_C1, ENC_A_C2, &pcntDer) &&
               configurarEncoder(ENC_B_C1, ENC_B_C2, &pcntIzq);
  Serial.println(encOk ? "  Encoders OK (PCNT)" : "  ERROR configurando encoders");

  // Ultrasonicos: TRIG sale, cada ECHO entra con su interrupcion
  pinMode(TRIG, OUTPUT);
  digitalWrite(TRIG, LOW);
  for (int i = 0; i < US_NUM; i++) {
    pinMode(ultrasonicos[i].pinEcho, INPUT);
    attachInterruptArg(ultrasonicos[i].pinEcho, isrEco, &ultrasonicos[i], CHANGE);
  }

  pinMode(BOTON_BOOT, INPUT_PULLUP);

  // I2C rapido (400 kHz): leer el giro tarda ~0.2 ms
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  hayGiro = iniciarMPU();
  if (hayGiro) {
    Serial.println("  Calibrando giroscopio: NO muevas el robot...");
    hayGiro = calibrarGiro();
  }
  if (!hayGiro) {
    Serial.println("  AVISO: sin MPU6050, el rumbo se estimara con los encoders (menos preciso)");
    rumboEncAnterior = rumboEncoders();
  }
  tGiroUs = micros();
  rumbo = 0;
  rumboObjetivo = 0;

  imprimirAyuda();
}

void loop() {
  servicioFondo();
  servicioSerie();

  if (botonPresionado()) {
    while (botonPresionado()) servicioFondo();   // esperar a que suelten el boton
    esperarMs(800);                              // quitar la mano del robot
    rumboObjetivo = rumbo;                       // "recto" = hacia donde apunta ahora
    avanzarCeldas(1);
  }
}
