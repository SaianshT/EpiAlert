#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADXL345_U.h>
#include <Adafruit_TMP117.h>
#include "MAX30105.h"
#include <Seizure_detection_inferencing.h>
#include <math.h>
#include <Arduino.h>

// ---------------- USER CONFIGURATION ----------------
const char EMERGENCY_PHONE_NUMBER[] = "+91XXXXXXXXXX";

#if defined(ESP32)
  #define GSM_RX_PIN 16
  #define GSM_TX_PIN 17
#endif
// ---------------------------------------------------

Adafruit_ADXL345_Unified accel = Adafruit_ADXL345_Unified(12345);
MAX30105 particleSensor;
Adafruit_TMP117 tmp117;

// Hardware Pin Definitions
#define COMM_LED    11   // Comm indicator LED
#define STATUS_LED  10   // System status LED
#define AUDIO_PIN   A0   // DAC -> PAM8403 Speaker Amplifier

// Edge Impulse ML Buffer
float features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
int feature_ix = 0;

// Alert State Engine
const float SEIZURE_THRESHOLD = 0.25f; // Pure ML confidence trigger threshold
unsigned long lastAlertTime = 0;
const unsigned long ALERT_COOLDOWN = 30000;
bool alertActive = false;
unsigned long alertStartTime = 0;
const unsigned long ALERT_DURATION = 10000;

void clearGSMBuffer() {
  while (Serial1.available()) {
    Serial.write(Serial1.read());
  }
}

void sendATCommand(const char* cmd, unsigned long delayMs = 300) {
  Serial1.println(cmd);
  delay(delayMs);
}

void dispatchEmergencyAlert() {
  Serial.println("\n[GSM] --- INITIATING EMERGENCY DISPATCH ---");
  clearGSMBuffer();

  // 1. Send SMS
  Serial.println("[GSM] Sending SMS...");
  Serial1.print("AT+CMGF=1\r\n");
  delay(300);
  clearGSMBuffer();

  Serial1.print("AT+CMGS=\"");
  Serial1.print(EMERGENCY_PHONE_NUMBER);
  Serial1.print("\"\r\n");
  delay(800);
  clearGSMBuffer();

  Serial1.print("Alert from EpiAlert");
  Serial1.write(0x1A); // Ctrl+Z
  
  // Wait for modem to complete SMS transmission
  unsigned long startWait = millis();
  while (millis() - startWait < 6000) {
    while (Serial1.available()) {
      Serial.write(Serial1.read());
    }
  }

  // 2. Clear buffer & verify modem ready
  Serial1.print("AT\r\n");
  delay(300);
  clearGSMBuffer();

  // 3. Place Voice Call
  Serial.println("\n[GSM] Dialing Emergency Contact...");
  Serial1.print("ATD");
  Serial1.print(EMERGENCY_PHONE_NUMBER);
  Serial1.print(";\r\n");

  // Ring recipient for 20 seconds
  startWait = millis();
  while (millis() - startWait < 20000) {
    while (Serial1.available()) {
      Serial.write(Serial1.read());
    }
  }

  // 4. Hang up
  Serial1.print("ATH\r\n");
  delay(300);
  clearGSMBuffer();
  Serial.println("\n[GSM] Alert sequence complete.");
}

void playAlertTone() {
  analogWriteResolution(10);
  unsigned long now = millis();
  int freq = ((now / 300) % 2) ? 880 : 440;
  int period = 1000000 / freq;
  
  for (int i = 0; i < 500; i++) {
    analogWrite(AUDIO_PIN, 1023);
    delayMicroseconds(period / 2);
    analogWrite(AUDIO_PIN, 0);
    delayMicroseconds(period / 2);
  }
}

void stopAlertTone() {
  analogWrite(AUDIO_PIN, 512); // Midpoint DAC = silence
}

void triggerAlert(float confidence) {
  Serial.println("==============================================");
  Serial.println("!!! SEIZURE DETECTED VIA EDGE IMPULSE !!!");
  Serial.print("ML Seizure Confidence: "); Serial.print(confidence * 100.0f); Serial.println("%");
  Serial.print("Timestamp: "); Serial.print(millis() / 1000); Serial.println("s");
  Serial.println("==============================================");

  alertActive = true;
  alertStartTime = millis();

  // Visual LED Flash Sequence
  for (int i = 0; i < 6; i++) {
    digitalWrite(COMM_LED, HIGH);
    delay(80);
    digitalWrite(COMM_LED, LOW);
    delay(80);
  }

  // Dispatch Emergency GSM Alert (SMS + Call)
  dispatchEmergencyAlert();
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000);
  Wire.begin();

  // Initialize GSM UART
  #if defined(ESP32)
    Serial1.begin(9600, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
  #else
    Serial1.begin(9600);
  #endif

  pinMode(COMM_LED, OUTPUT);
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(COMM_LED, LOW);
  digitalWrite(STATUS_LED, LOW);

  // Initialize ADXL345 for ML inference
  if (!accel.begin()) {
    Serial.println("ADXL345 init failed");
    while (1);
  }
  accel.setRange(ADXL345_RANGE_4_G);
  Serial.println("ADXL345 OK");

  // Optional sensor inits (passive telemetry)
  if (tmp117.begin(0x49)) Serial.println("TMP117 OK (0x49)");
  if (particleSensor.begin(Wire)) {
    particleSensor.setup();
    Serial.println("MAX30102 OK");
  }

  // System Ready Indicator
  digitalWrite(STATUS_LED, HIGH);

  Serial.println("EpiAlert ML Inference Armed (Trigger: Edge Impulse Exclusively)");
  Serial.print("ML Feature Buffer Size: ");
  Serial.println(EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);
}

void loop() {
  // --- HANDLE ACTIVE SIREN & LED ---
  if (alertActive) {
    playAlertTone();
    digitalWrite(COMM_LED, (millis() / 250) % 2);

    if (millis() - alertStartTime > ALERT_DURATION) {
      alertActive = false;
      stopAlertTone();
      digitalWrite(COMM_LED, LOW);
      Serial.println("Alert sequence completed.");
    }
  }

  // --- ACCELEROMETER DATA STREAM & FEATURE BUFFER ---
  sensors_event_t event;
  accel.getEvent(&event);
  float x = event.acceleration.x / 9.81f;
  float y = event.acceleration.y / 9.81f;
  float z = event.acceleration.z / 9.81f;

  if (feature_ix + 3 <= EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
    features[feature_ix++] = x;
    features[feature_ix++] = y;
    features[feature_ix++] = z;
  }

  // --- RUN EDGE IMPULSE INFERENCE ---
  if (feature_ix >= EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
    feature_ix = 0;

    signal_t signal;
    int err = numpy::signal_from_buffer(features, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
    if (err != 0) {
      Serial.print("Signal error: "); Serial.println(err);
      return;
    }

    ei_impulse_result_t result;
    err = run_classifier(&signal, &result, false);
    if (err != EI_IMPULSE_OK) {
      Serial.print("Classifier error: "); Serial.println(err);
      return;
    }

    float normal_conf  = result.classification[0].value;
    float seizure_conf = result.classification[1].value;

    Serial.print("Normal: "); Serial.print(normal_conf, 3);
    Serial.print(" | Seizure: "); Serial.println(seizure_conf, 3);

    // --- SOLE TRIGGER CONDITION: EDGE IMPULSE ML CLASSIFICATION ---
    unsigned long now = millis();
    if (seizure_conf > SEIZURE_THRESHOLD && (now - lastAlertTime) > ALERT_COOLDOWN) {
      lastAlertTime = now;
      triggerAlert(seizure_conf);
    }
  }

  delay(10);
}