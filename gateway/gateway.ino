
/*
  B31OT COOPERATIVE TEMPERATURE MONITORING
  ESP32-C3 GATEWAY FIRMWARE

  Arduino-ESP32: 3.3.12
  ArduinoJson: 7
  ESP-NOW channel: 1

  USB serial: 115200 baud
  Gateway remains awake.

  Receives sensor telemetry.
  Sends application ACKs.
  Coordinates collective alarm and fair indicator rotation.
  Queues configuration for sleeping sensors.
*/

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>

#define SERIAL_BAUD 115200
#define ESPNOW_CHANNEL 1
#define SENSOR_COUNT 2

#define DEFAULT_THRESHOLD_C 40.0f
#define SENSOR_OFFLINE_MS 15000
#define INDICATOR_ROTATION_MS 30000

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


/* ---------------- SENSOR STATE ---------------- */

uint8_t sensorMacs[SENSOR_COUNT + 1][6];

bool sensorKnown[SENSOR_COUNT + 1] = {
  false, false, false
};

bool sensorOnline[SENSOR_COUNT + 1] = {
  false, false, false
};

bool sensorAlarm[SENSOR_COUNT + 1] = {
  false, false, false
};

float sensorThreshold[SENSOR_COUNT + 1] = {
  0,
  DEFAULT_THRESHOLD_C,
  DEFAULT_THRESHOLD_C
};

float lastTemperature[SENSOR_COUNT + 1] = {0, 0, 0};

unsigned long lastSeen[SENSOR_COUNT + 1] = {0, 0, 0};

uint16_t lastTelemetrySequence[SENSOR_COUNT + 1] = {0, 0, 0};

bool hasTelemetrySequence[SENSOR_COUNT + 1] = {
  false, false, false
};


/* ---------------- CONFIGURATION QUEUE ---------------- */

float desiredThreshold[SENSOR_COUNT + 1] = {
  0,
  DEFAULT_THRESHOLD_C,
  DEFAULT_THRESHOLD_C
};

bool desiredIndicator[SENSOR_COUNT + 1] = {
  false, false, false
};

bool configPending[SENSOR_COUNT + 1] = {
  false, false, false
};

uint16_t configSequence[SENSOR_COUNT + 1] = {0, 0, 0};

uint16_t commandSequence = 0;


/* ---------------- INDICATOR COORDINATION ---------------- */

int8_t indicatorOwner = -1;

uint8_t nextIndicator = 1;

unsigned long lastIndicatorRotationMs = 0;


/* ---------------- RECEIVED PACKET QUEUE ---------------- */

struct RxMessage {
  Packet packet;
  uint8_t sourceMac[6];
};

QueueHandle_t rxQueue;


/* ---------------- JSON SERIAL OUTPUT ---------------- */

void emitStatus(uint8_t id, const char *status) {

  JsonDocument doc;

  doc["type"] = "status";
  doc["sensor_id"] = id;
  doc["status"] = status;
  doc["timestamp_ms"] = millis();

  serializeJson(doc, Serial);
  Serial.println();
}

void emitCommandSent(uint8_t id, float threshold) {

  JsonDocument doc;

  doc["type"] = "command_sent";
  doc["sensor_id"] = id;
  doc["threshold_c"] = threshold;

  serializeJson(doc, Serial);
  Serial.println();
}

void emitTelemetry(
  uint8_t id,
  const Packet &p
) {

  JsonDocument doc;

  doc["type"] = "telemetry";
  doc["sensor_id"] = id;
  doc["sequence"] = p.seq;
  doc["temperature_c"] = p.temperatureC;
  doc["threshold_c"] = p.thresholdC;
  doc["alarm"] = p.alarm == 1;
  doc["indicator"] = p.indicator;
  doc["timestamp_ms"] = millis();

  serializeJson(doc, Serial);
  Serial.println();
}

void emitConfigAck(
  uint8_t id,
  const Packet &p
) {

  JsonDocument doc;

  doc["type"] = "config_ack";
  doc["sensor_id"] = id;
  doc["sequence"] = p.seq;
  doc["threshold_c"] = p.thresholdC;
  doc["alarm"] = p.alarm == 1;
  doc["indicator"] = p.indicator;
  doc["timestamp_ms"] = millis();

  serializeJson(doc, Serial);
  Serial.println();
}


/* ---------------- ESP-NOW PEERS ---------------- */

bool ensurePeer(const uint8_t *mac) {

  if (esp_now_is_peer_exist(mac)) {
    return true;
  }

  esp_now_peer_info_t peer = {};

  memcpy(peer.peer_addr, mac, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;

  return esp_now_add_peer(&peer) == ESP_OK;
}


/* ---------------- ACKNOWLEDGEMENT ---------------- */

void sendDataAck(
  uint8_t id,
  uint16_t sequence,
  const uint8_t *mac
) {

  Packet ack = {};

  ack.type = DATA_ACK;
  ack.sensorId = id;
  ack.seq = sequence;

  esp_now_send(
    mac,
    (uint8_t *)&ack,
    sizeof(ack)
  );
}


/* ---------------- CONFIGURATION MANAGEMENT ---------------- */

void queueConfig(
  uint8_t id,
  float threshold,
  bool indicator
) {

  if (id < 1 || id > SENSOR_COUNT) return;

  desiredThreshold[id] = threshold;
  desiredIndicator[id] = indicator;

  configSequence[id] = ++commandSequence;

  configPending[id] = true;
}

void sendPendingConfig(uint8_t id) {

  if (id < 1 || id > SENSOR_COUNT) return;

  if (!sensorKnown[id] || !configPending[id]) {
    return;
  }

  Packet p = {};

  p.type = SET_CONFIG;
  p.sensorId = id;
  p.seq = configSequence[id];
  p.thresholdC = desiredThreshold[id];
  p.indicator = desiredIndicator[id] ? 1 : 0;

  esp_err_t result = esp_now_send(
    sensorMacs[id],
    (uint8_t *)&p,
    sizeof(p)
  );

  if (result != ESP_OK) {

    Serial.printf(
      "WARNING: Configuration send failed for sensor %u: %d\n",
      id,
      (int)result
    );

  } else {

    Serial.printf(
      "Configuration sent to sensor %u; sequence=%u\n",
      id,
      p.seq
    );
  }
}


/* ---------------- COLLECTIVE ALARM ---------------- */

bool collectiveAlarmActive() {

  for (int id = 1; id <= SENSOR_COUNT; id++) {

    if (sensorOnline[id] && sensorAlarm[id]) {
      return true;
    }
  }

  return false;
}

void updateIndicatorAssignment() {

  bool anyAlarm = collectiveAlarmActive();

  // No alarms: clear the continuous indicator.
  if (!anyAlarm) {

    if (indicatorOwner != -1) {

      Serial.println("Collective alarm cleared");

      indicatorOwner = -1;

      for (int id = 1; id <= SENSOR_COUNT; id++) {

        if (sensorKnown[id]) {

          queueConfig(
            id,
            sensorThreshold[id],
            false
          );
        }
      }
    }

    return;
  }

  // Keep the current owner if it is still online
  // and its alarm is still active.
  if (indicatorOwner >= 1 &&
      indicatorOwner <= SENSOR_COUNT &&
      sensorOnline[indicatorOwner] &&
      sensorAlarm[indicatorOwner]) {

    return;
  }

  // Select the next eligible sensor.
  int chosen = -1;

  for (int k = 0; k < SENSOR_COUNT; k++) {

    int candidate =
      ((nextIndicator - 1 + k) % SENSOR_COUNT) + 1;

    if (sensorOnline[candidate] &&
        sensorAlarm[candidate]) {

      chosen = candidate;
      break;
    }
  }

  if (chosen == -1) return;

  indicatorOwner = chosen;

  nextIndicator = (chosen % SENSOR_COUNT) + 1;

  lastIndicatorRotationMs = millis();

  Serial.printf(
    "Collective alarm active; Sensor %d selected\n",
    chosen
  );

  // Send the new assignment to all known sensors.
  // Sleeping sensors receive it after their next telemetry.
  for (int id = 1; id <= SENSOR_COUNT; id++) {

    if (sensorKnown[id]) {

      queueConfig(
        id,
        sensorThreshold[id],
        id == indicatorOwner
      );
    }
  }
}


/* ---------------- FAIR INDICATOR ROTATION ---------------- */

void rotateIndicatorIfDue() {

  unsigned long now = millis();

  if (now - lastIndicatorRotationMs <
      INDICATOR_ROTATION_MS) {
    return;
  }

  int activeAlarmCount = 0;

  for (int id = 1; id <= SENSOR_COUNT; id++) {

    if (sensorOnline[id] && sensorAlarm[id]) {
      activeAlarmCount++;
    }
  }

  // Rotation is only necessary when both sensors
  // have active alarms.
  if (activeAlarmCount < 2) return;

  int chosen = -1;

  for (int k = 0; k < SENSOR_COUNT; k++) {

    int candidate =
      ((nextIndicator - 1 + k) % SENSOR_COUNT) + 1;

    if (candidate != indicatorOwner &&
        sensorOnline[candidate] &&
        sensorAlarm[candidate]) {

      chosen = candidate;
      break;
    }
  }

  if (chosen == -1) return;

  indicatorOwner = chosen;

  nextIndicator = (chosen % SENSOR_COUNT) + 1;

  lastIndicatorRotationMs = now;

  Serial.printf(
    "Fair rotation: Sensor %d is now indicator owner\n",
    chosen
  );

  for (int id = 1; id <= SENSOR_COUNT; id++) {

    if (sensorKnown[id]) {

      queueConfig(
        id,
        sensorThreshold[id],
        id == indicatorOwner
      );
    }
  }
}


/* ---------------- PROCESS RECEIVED PACKETS ---------------- */

void processReceivedPacket(const RxMessage &message) {

  Packet p = message.packet;

  uint8_t id = p.sensorId;

  if (id < 1 || id > SENSOR_COUNT) return;

  memcpy(sensorMacs[id], message.sourceMac, 6);

  sensorKnown[id] = true;

  ensurePeer(message.sourceMac);

  sensorOnline[id] = true;
  lastSeen[id] = millis();

  if (p.type == TELEMETRY) {

    // ACK every received packet, including duplicates.
    sendDataAck(id, p.seq, message.sourceMac);

    // Ignore duplicate telemetry after acknowledging it.
    if (hasTelemetrySequence[id] &&
        p.seq == lastTelemetrySequence[id]) {

      return;
    }

    lastTelemetrySequence[id] = p.seq;
    hasTelemetrySequence[id] = true;

    lastTemperature[id] = p.temperatureC;
    sensorAlarm[id] = p.alarm == 1;

    // Do not overwrite a pending remote threshold command.
    if (!configPending[id]) {

      sensorThreshold[id] = p.thresholdC;
      desiredThreshold[id] = p.thresholdC;
    }

    emitTelemetry(id, p);

    // Check collective alarm and indicator assignment.
    updateIndicatorAssignment();

    // If this sensor has a queued configuration,
    // send it while the sensor is awake.
    sendPendingConfig(id);

  } else if (p.type == CONFIG_ACK) {

    // Only clear a pending command if the sequence matches.
    if (configPending[id] &&
        p.seq == configSequence[id]) {

      configPending[id] = false;

      sensorThreshold[id] = p.thresholdC;
      desiredThreshold[id] = p.thresholdC;
      desiredIndicator[id] = p.indicator == 1;

      Serial.printf(
        "Sensor %u applied configuration successfully\n",
        id
      );
    }

    emitConfigAck(id, p);
  }
}


/* ---------------- ESP-NOW CALLBACKS ---------------- */

void onDataRecv(
  const esp_now_recv_info_t *info,
  const uint8_t *data,
  int len
) {

  if (len != sizeof(Packet)) return;

  RxMessage message;

  memcpy(
    &message.packet,
    data,
    sizeof(Packet)
  );

  memcpy(
    message.sourceMac,
    info->src_addr,
    6
  );

  // Keep callback work short. Process packets in loop().
  if (rxQueue != nullptr) {
    xQueueSend(rxQueue, &message, 0);
  }
}

void onDataSent(
  const wifi_tx_info_t *info,
  esp_now_send_status_t status
) {
  // Application-level acknowledgement is handled
  // using DATA_ACK and CONFIG_ACK packets.
}


/* ---------------- SERIAL COMMANDS ---------------- */

void handleSerialCommand(const String &line) {

  JsonDocument doc;

  DeserializationError err =
    deserializeJson(doc, line);

  if (err) {

    Serial.println(
      "{\"type\":\"error\",\"message\":\"Invalid JSON command\"}"
    );

    return;
  }

  const char *cmd = doc["cmd"] | "";

  if (strcmp(cmd, "SET_THRESHOLD") == 0) {

    int id = doc["sensor_id"] | 0;

    float threshold =
      doc["threshold_c"] | DEFAULT_THRESHOLD_C;

    if (id < 1 ||
        id > SENSOR_COUNT ||
        !sensorKnown[id] ||
        !isfinite(threshold) ||
        threshold < 0 ||
        threshold > 100) {

      Serial.println(
        "{\"type\":\"command_error\",\"message\":\"Sensor unknown or invalid threshold/ID\"}"
      );

      return;
    }

    // Preserve the current desired indicator assignment.
    queueConfig(
      (uint8_t)id,
      threshold,
      desiredIndicator[id]
    );

    emitCommandSent(
      (uint8_t)id,
      threshold
    );

    // The sensor may be sleeping.
    // Its configuration will be sent after its next telemetry.
  } else {

    Serial.println(
      "{\"type\":\"command_error\",\"message\":\"Unsupported command\"}"
    );
  }
}


/* ---------------- SETUP ---------------- */

void setup() {

  Serial.begin(SERIAL_BAUD);
  delay(500);

  rxQueue = xQueueCreate(
    10,
    sizeof(RxMessage)
  );

  if (rxQueue == nullptr) {

    Serial.println(
      "{\"type\":\"error\",\"message\":\"Could not create packet queue\"}"
    );

    while (true) delay(1000);
  }

  WiFi.mode(WIFI_STA);

  esp_wifi_set_channel(
    ESPNOW_CHANNEL,
    WIFI_SECOND_CHAN_NONE
  );

  if (esp_now_init() != ESP_OK) {

    Serial.println(
      "{\"type\":\"error\",\"message\":\"ESP-NOW init failed\"}"
    );

    while (true) delay(1000);
  }

  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);

  Serial.printf(
    "{\"type\":\"gateway_ready\",\"mac\":\"%s\",\"channel\":%d}\n",
    WiFi.macAddress().c_str(),
    ESPNOW_CHANNEL
  );
}


/* ---------------- MAIN LOOP ---------------- */

void loop() {

  static String input;

  // Read commands from the Python MQTT bridge.
  while (Serial.available()) {

    char c = (char)Serial.read();

    if (c == '\n') {

      input.trim();

      if (input.length()) {
        handleSerialCommand(input);
      }

      input = "";

    } else if (input.length() < 300) {

      input += c;
    }
  }

  // Process incoming ESP-NOW packets.
  RxMessage message;

  while (xQueueReceive(
    rxQueue,
    &message,
    0
  ) == pdTRUE) {

    processReceivedPacket(message);
  }

  unsigned long now = millis();

  // Detect sensors that have stopped reporting.
  for (int id = 1; id <= SENSOR_COUNT; id++) {

    if (sensorOnline[id] &&
        now - lastSeen[id] > SENSOR_OFFLINE_MS) {

      sensorOnline[id] = false;

      emitStatus(id, "offline");

      Serial.printf(
        "Sensor %u marked offline\n",
        id
      );

      if (indicatorOwner == id) {
        indicatorOwner = -1;
      }

      updateIndicatorAssignment();
    }
  }

  // Fairly rotate the indicator when both sensors
  // have active alarms.
  rotateIndicatorIfDue();

  delay(2);
}
