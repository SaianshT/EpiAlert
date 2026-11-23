
#include <Arduino_LSM9DS1.h>

#include <modulg-project-1_inference.h>
#include <ArduinoBLE.h>

BLEService predictionService("382d6dfe-a1b3-11eb-bcbc-0242ac130002");

BLEUnsignedCharCharacteristic predictionChar("382d6dff-a1b3-11eb-bcbc-0242ac130002", 
    BLERead | BLENotify); 


#define CONVERT_G_TO_MS2    9.80665f

static bool debug_nn = false; 
static uint32_t run_inference_every_ms = 200;
static rtos::Thread inference_thread(osPriorityLow);
static float buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE] = { 0 };
static float inference_buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
int counter = 0;
int indicator = 0;
float oldPrediction = 0;
long previousMillis = 0;
long previousUpdateMillis = 0;

void updatePrediction();
void run_inference_background();

void setup()
{
  Serial.begin(115200);
  Serial.println("Edge Impulse working");


  pinMode(LED_BUILTIN, OUTPUT); 

  if (!BLE.begin()) {
    Serial.println("starting BLE failed!");

    while (1);
  }

  if (!IMU.begin()) {
    ei_printf("Failed to initialize IMU!\r\n");
  }
  else {
    ei_printf("IMU initialized\r\n");
  }

  if (EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME != 3) {
    ei_printf("ERR: EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME should be equal to 3 (the 3 sensor axes)\n");
    return;
  }

  inference_thread.start(mbed::callback(&run_inference_background));

  BLE.setLocalName("EpiAlert");
  BLE.setAdvertisedService(predictionService);

  predictionService.addCharacteristic(predictionChar);
  BLE.addService(predictionService);
  predictionChar.writeValue(oldPrediction);

  BLE.advertise();

  Serial.println("Bluetooth ON, waiting connecting...");
}

void ei_printf(const char *format, ...) {
  static char print_buf[1024] = { 0 };

  va_list args;
  va_start(args, format);
  int r = vsnprintf(print_buf, sizeof(print_buf), format, args);
  va_end(args);

  if (r > 0) {
    Serial.write(print_buf);
  }
}

void run_inference_background()
{
  delay((EI_CLASSIFIER_INTERVAL_MS * EI_CLASSIFIER_RAW_SAMPLE_COUNT) + 100);

  ei_classifier_smooth_t smooth;
  ei_classifier_smooth_init(&smooth, 10 , 7 , 0.8 , 0.3 );

  while (1) {
    memcpy(inference_buffer, buffer, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE * sizeof(float));

    signal_t signal;
    int err = numpy::signal_from_buffer(inference_buffer, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
    if (err != 0) {
      ei_printf("Failed to create signal from buffer (%d)\n", err);
      return;
    }

    ei_impulse_result_t result = { 0 };

    err = run_classifier(&signal, &result, debug_nn);
    if (err != EI_IMPULSE_OK) {
      ei_printf("ERR: Failed to run classifier (%d)\n", err);
      return;
    }

    ei_printf("Predictions ");
    ei_printf("(DSP: %d ms., Classification: %d ms., Anomaly: %d ms.)",
              result.timing.dsp, result.timing.classification, result.timing.anomaly);
    ei_printf(": ");

    const char *prediction = ei_classifier_smooth_update(&smooth, &result);

    if (*prediction == '1') {
      counter++;
      if (counter == 12) {
        ei_printf("SEIZURE\n");
        indicator = 1;
        counter = 0;
        long currentMillis = millis();
        if (currentMillis - previousMillis >= 200) {
          previousMillis = currentMillis;
          updatePrediction();
        }
      }

    }
    else {
      counter = 0;
      ei_printf("no seizure\n");
      indicator = 0;
      long currentMillis = millis();
      if (currentMillis - previousMillis >= 200) {
        previousMillis = currentMillis;
        updatePrediction();
      }

    }

    ei_printf("%s ", prediction);
    ei_printf(" [ ");
    for (size_t ix = 0; ix < smooth.count_size; ix++) {
      ei_printf("%u", smooth.count[ix]);
      if (ix != smooth.count_size + 1) {
        ei_printf(", ");
      }
      else {
        ei_printf(" ");
      }
    }
    ei_printf("]\n");

    delay(run_inference_every_ms);
  }

  ei_classifier_smooth_free(&smooth);
}

void loop()
{
  // wait for a BLE central
  BLEDevice central = BLE.central();
  if (central) {
    Serial.print("Connected to central: ");
    Serial.println(central.address());
    digitalWrite(LED_BUILTIN, HIGH);

    while (central.connected()) {
      long currentMillis = millis();
      if (currentMillis - previousMillis >= 200) {
        previousMillis = currentMillis;
        updatePrediction();
      }

      uint64_t next_tick = micros() + (EI_CLASSIFIER_INTERVAL_MS * 1000);

      numpy::roll(buffer, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, -3);

      IMU.readAcceleration(
        buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE - 3],
        buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE - 2],
        buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE - 1]
      );

      buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE - 3] *= CONVERT_G_TO_MS2;
      buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE - 2] *= CONVERT_G_TO_MS2;
      buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE - 1] *= CONVERT_G_TO_MS2;

      uint64_t time_to_wait = next_tick - micros();
      delay((int)floor((float)time_to_wait / 1000.0f));
      delayMicroseconds(time_to_wait % 1000);
    }
    digitalWrite(LED_BUILTIN, LOW);
    Serial.print("Disconnected from central: ");
    Serial.println(central.address());
  }

}

void updatePrediction() {
  float prediction = indicator;
  long currentMillis = millis();

  if (prediction != oldPrediction || (currentMillis - previousUpdateMillis) >= 5000) {      // if the battery level has changed
    previousUpdateMillis = currentMillis;
    predictionChar.writeValue(prediction);  
    oldPrediction = prediction;           
  }
}
