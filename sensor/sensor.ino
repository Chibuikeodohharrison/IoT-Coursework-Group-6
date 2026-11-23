
/*
  B31OT COOPERATIVE TEMPERATURE MONITORING
  ESP32-C3 SENSOR FIRMWARE - LIGHT SLEEP

  Arduino-ESP32 core: 3.3.12
  ESP-NOW channel: 1

  Sensor 1: #define SENSOR_ID 1
  Sensor 2: #define SENSOR_ID 2

  Uses the ESP32-C3 internal chip temperature sensor.
*/

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <esp_timer.h>

#define SENSOR_ID 2              // CHANGE TO 2 ON SENSOR 2

#define LED_PIN 8
#define LED_ACTIVE_HIGH true

#define ESPNOW_CHANNEL 1
#define SAMPLE_INTERVAL_MS 5000
#define ACK_TIMEOUT_MS 400
#define MAX_RETRIES 3
#define CONFIG_LISTEN_MS 500

#define DEFAULT_THRESHOLD_C 40.0f
#define HYSTERESIS_C 0.5f

uint8_t gatewayMac[6] = {
  0x70, 0xAF, 0x09, 0x0D, 0xDC, 0x74
};

enum PacketType : uint8_t {
  TELEMETRY = 1,
  DATA_ACK = 2,
  SET_CONFIG = 3,
  CONFIG_ACK = 4
};

typedef struct __attribute__((packed)) {
  uint8_t type;
  uint8_t sensorId;
  uint16_t seq;
  float temperatureC;
  float thresholdC;
  uint8_t alarm;
  int8_t indicator;
} Packet;

Packet outgoing;

uint16_t sequenceNumber = 0;
uint16_t waitingForAck = 0;

volatile bool ackReceived = false;
volatile uint16_t acknowledgedSequence = 0;
volatile bool configWasUpdated = false;

float thresholdC = DEFAULT_THRESHOLD_C;
float latestTemperatureC = 0.0f;

bool localAlarm = false;
bool continuousIndicator = false;
bool radioReady = false;


/* ---------------- LED CONTROL ---------------- */

void setLed(bool on) {
  digitalWrite(
    LED_PIN,
    (on == LED_ACTIVE_HIGH) ? HIGH : LOW
  );
}

void sampleBlink() {

  // The selected collective-alarm sensor stays continuously lit.
  if (continuousIndicator) {
    setLed(true);
    return;
  }

  setLed(true);
  delay(100);
  setLed(false);
}


/* ---------------- ESP-NOW CALLBACKS ---------------- */

void onDataSent(
  const wifi_tx_info_t *info,
  esp_now_send_status_t status
) {
  // Application-level ACK is handled by onDataRecv().
}

void onDataRecv(
  const esp_now_recv_info_t *info,
  const uint8_t *data,
  int len
) {

  if (len != sizeof(Packet)) return;

  Packet p;
  memcpy(&p, data, sizeof(p));

  if (p.sensorId != SENSOR_ID) return;

  if (p.type == DATA_ACK) {

    if (p.seq == waitingForAck) {
      acknowledgedSequence = p.seq;
      ackReceived = true;
    }

  } else if (p.type == SET_CONFIG) {

    // Apply the new threshold and indicator assignment.
    if (isfinite(p.thresholdC) &&
        p.thresholdC >= 0 &&
        p.thresholdC <= 100) {

      thresholdC = p.thresholdC;
    }

    continuousIndicator = (p.indicator == 1);

    setLed(continuousIndicator);

    // Confirm that this sensor applied the configuration.
    Packet response = {};

    response.type = CONFIG_ACK;
    response.sensorId = SENSOR_ID;
    response.seq = p.seq;
    response.temperatureC = latestTemperatureC;
    response.thresholdC = thresholdC;
    response.alarm = localAlarm ? 1 : 0;
    response.indicator = continuousIndicator ? 1 : 0;

    esp_now_send(
      gatewayMac,
      (uint8_t *)&response,
      sizeof(response)
    );

    configWasUpdated = true;
  }
}


/* ---------------- RADIO START/STOP ---------------- */

bool initialiseEspNow() {

  if (esp_wifi_set_channel(
        ESPNOW_CHANNEL,
        WIFI_SECOND_CHAN_NONE
      ) != ESP_OK) {
    return false;
  }

  if (esp_now_init() != ESP_OK) {
    return false;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peer = {};

  memcpy(peer.peer_addr, gatewayMac, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;

  if (esp_now_add_peer(&peer) != ESP_OK) {
    esp_now_deinit();
    return false;
  }

  radioReady = true;
  return true;
}

bool restartRadioAfterSleep() {

  // Wi-Fi driver was stopped before light sleep.
  if (esp_wifi_start() != ESP_OK) {
    Serial.println("ERROR: Wi-Fi restart failed");
    return false;
  }

  delay(20);

  if (!initialiseEspNow()) {
    Serial.println("ERROR: ESP-NOW restart failed");
    return false;
  }

  return true;
}

void stopRadioBeforeSleep() {

  if (radioReady) {
    esp_now_deinit();
    radioReady = false;
  }

  esp_wifi_stop();
}


/* ---------------- ALARM PROCESSING ---------------- */

void updateLocalAlarm() {

  // Hysteresis prevents repeated alarm transitions
  // around the threshold.
  if (!localAlarm &&
      latestTemperatureC > thresholdC) {

    localAlarm = true;

    Serial.println("LOCAL ALARM ACTIVE");

  } else if (localAlarm &&
             latestTemperatureC <
             thresholdC - HYSTERESIS_C) {

    localAlarm = false;

    Serial.println("LOCAL ALARM CLEARED");
  }
}


/* ---------------- TELEMETRY AND ACK ---------------- */

bool sendTelemetryWithRetry() {

  outgoing.type = TELEMETRY;
  outgoing.sensorId = SENSOR_ID;
  outgoing.seq = ++sequenceNumber;
  outgoing.temperatureC = latestTemperatureC;
  outgoing.thresholdC = thresholdC;
  outgoing.alarm = localAlarm ? 1 : 0;
  outgoing.indicator = continuousIndicator ? 1 : 0;

  waitingForAck = outgoing.seq;

  ackReceived = false;
  acknowledgedSequence = 0;

  // Initial transmission plus up to three retries.
  for (uint8_t attempt = 0;
       attempt <= MAX_RETRIES;
       attempt++) {

    esp_err_t result = esp_now_send(
      gatewayMac,
      (uint8_t *)&outgoing,
      sizeof(outgoing)
    );

    if (result != ESP_OK) {
      Serial.printf(
        "ESP-NOW send error: %d\n",
        (int)result
      );
    }

    unsigned long start = millis();

    while (millis() - start < ACK_TIMEOUT_MS) {

      if (ackReceived &&
          acknowledgedSequence == waitingForAck) {

        Serial.printf(
          "ACK received: sensor=%u sequence=%u\n",
          SENSOR_ID,
          waitingForAck
        );

        return true;
      }

      delay(5);
    }

    Serial.printf(
      "ACK timeout: sensor=%u sequence=%u attempt=%u\n",
      SENSOR_ID,
      waitingForAck,
      attempt + 1
    );
  }

  Serial.println("WARNING: Telemetry was not acknowledged");
  return false;
}


/* ---------------- CONFIGURATION LISTENING ---------------- */

void listenForConfiguration() {

  unsigned long start = millis();

  while (millis() - start < CONFIG_LISTEN_MS) {
    delay(5);
  }

  if (configWasUpdated) {

    Serial.printf(
      "Configuration applied: sensor=%u threshold=%.2f indicator=%u\n",
      SENSOR_ID,
      thresholdC,
      continuousIndicator ? 1 : 0
    );

    configWasUpdated = false;
  }
}


/* ---------------- LIGHT SLEEP ---------------- */

void enterLightSleep(uint64_t cycleStartUs) {

  const uint64_t intervalUs =
    (uint64_t)SAMPLE_INTERVAL_MS * 1000ULL;

  uint64_t elapsedUs =
    (uint64_t)(esp_timer_get_time() - cycleStartUs);

  if (elapsedUs >= intervalUs) {

    Serial.println(
      "WARNING: Cycle exceeded five seconds; skipping sleep"
    );

    return;
  }

  uint64_t sleepUs = intervalUs - elapsedUs;

  Serial.printf(
    "Light sleep: %.3f seconds\n",
    sleepUs / 1000000.0
  );

  stopRadioBeforeSleep();

  esp_sleep_enable_timer_wakeup(sleepUs);

  esp_err_t result = esp_light_sleep_start();

  if (result != ESP_OK) {
    Serial.printf(
      "Light sleep error: %d\n",
      (int)result
    );
  }

  Serial.println("Sensor awake again");

  if (!restartRadioAfterSleep()) {
    Serial.println("ERROR: Radio restart failed");
  }
}


/* ---------------- SETUP ---------------- */

void setup() {

  Serial.begin(115200);
  delay(300);

  pinMode(LED_PIN, OUTPUT);
  setLed(false);

  // Start Wi-Fi once. It will be stopped before sleep
  // and restarted using esp_wifi_start() after waking.
  WiFi.mode(WIFI_STA);

  Serial.printf(
    "\nSensor %u ready; MAC=%s; channel=%u\n",
    SENSOR_ID,
    WiFi.macAddress().c_str(),
    ESPNOW_CHANNEL
  );

  if (!initialiseEspNow()) {

    Serial.println("ERROR: Initial ESP-NOW setup failed");

    while (true) {
      delay(1000);
    }
  }
}


/* ---------------- MAIN LOOP ---------------- */

void loop() {

  // Measure each cycle from its start.
  uint64_t cycleStartUs = esp_timer_get_time();

  // Sample the ESP32-C3 internal chip temperature.
  latestTemperatureC = temperatureRead();

  updateLocalAlarm();

  // Brief sample LED blink, unless this node owns
  // the continuous collective-alarm indicator.
  sampleBlink();

  Serial.printf(
    "Sample: sensor=%u sequence=%u temperature=%.2f threshold=%.2f alarm=%u\n",
    SENSOR_ID,
    sequenceNumber + 1,
    latestTemperatureC,
    thresholdC,
    localAlarm ? 1 : 0
  );

  // Send reading and wait for ACK/retries.
  sendTelemetryWithRetry();

  // The gateway can send queued threshold and indicator
  // configuration during this listening window.
  listenForConfiguration();

  // Sleep for the remainder of the five-second cycle.
  enterLightSleep(cycleStartUs);
}
