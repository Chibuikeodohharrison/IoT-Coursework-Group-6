# B31OT Cooperative Temperature Monitoring - Complete Starter Package

## Assignment architecture
Two ESP32-C3 sensor nodes send ESP-NOW packets to one USB-connected ESP32-C3 gateway.
The gateway emits newline-delimited JSON over USB serial. The laptop Python program is
the Serial-MQTT bridge: it publishes gateway telemetry to Mosquitto and forwards
dashboard threshold commands back to the gateway. Node-RED subscribes to MQTT.

This follows the assignment diagram on page 1 of the B31OT brief.

## Package contents
- `sensor.ino` - upload to both sensor boards; set SENSOR_ID to 1 or 2.
- `gateway.ino` - upload to the USB-connected gateway.
- `serial_mqtt_bridge.py` - laptop serial/MQTT bridge.
- `requirements.txt` - Python dependencies.
- `mosquitto.conf.example` - example local broker configuration.
- `README.txt` - these instructions.

## Hardware and Arduino IDE
1. Install Arduino IDE and Espressif ESP32 board support.
2. Select your exact ESP32-C3 board and its COM port.
3. Use Arduino-ESP32 core 3.x for these callback signatures.
4. Install ArduinoJson 7 in Library Manager (gateway only).
5. Keep ESP-NOW channel at 1 on all three boards.
6. Check `LED_PIN` and LED polarity in `sensor.ino` against your exact board.
7. Gateway STA MAC must be `70:AF:09:0D:DC:74` for the current sensor sketch.
   Verify it in the gateway serial output. If it differs, change `gatewayMac[]`
   in sensor.ino to the actual gateway MAC bytes.

## Upload order
1. Upload `gateway.ino` to the gateway.
2. Open Serial Monitor at 115200 baud and note the gateway MAC.
3. Upload `sensor.ino` to Sensor 1 with `#define SENSOR_ID 1`.
4. Change the define to 2 and upload to Sensor 2.
5. Close Serial Monitor before running Python because only one program can own
   the COM port at a time.
6. Connect all boards by USB power. Sensors and gateway should use channel 1.

## Mosquitto on Windows
If Mosquitto is already running, keep your working configuration. The example
configuration uses:
  listener 1883 0.0.0.0
  allow_anonymous true

This is only for a trusted private lab network. Do not expose it to the public
internet. For a shared/untrusted network, configure a password file and firewall.

Your laptop broker address for this project is `10.128.105.9`, port `1883`.
For Python running on the same laptop, use `127.0.0.1` as broker address.
For other computers on the LAN, use `10.128.105.9`.

Test broker in two Command Prompt windows:
  mosquitto_sub -h 127.0.0.1 -t iot/test -v
  mosquitto_pub -h 127.0.0.1 -t iot/test -m "Hello MQTT"

## Python setup
Open Command Prompt in this package folder:
  python --version
  python -m pip install -r requirements.txt
Find the gateway COM port:
  python serial_mqtt_bridge.py --list-ports

Run the bridge (replace COM5 with the actual gateway port):
  python serial_mqtt_bridge.py --port COM5 --broker 127.0.0.1

Important:
- Close Arduino Serial Monitor before starting the bridge.
- If using a different laptop as broker, use `--broker 10.128.105.9`.
- Windows Firewall must permit inbound TCP 1883 if broker clients connect over LAN.
- The Python bridge must remain running for MQTT data and commands to flow.

## MQTT topics and payloads
Telemetry:
  iot/sensor/1/data
  iot/sensor/2/data

Alarm:
  iot/sensor/1/alarm
  iot/sensor/2/alarm

Status:
  iot/sensor/1/status
  iot/sensor/2/status
  iot/gateway/status

Threshold command (publish JSON to `iot/command/threshold`):
  {"sensor_id":1,"threshold_c":38.0}

The gateway forwards the command to the selected sensor over ESP-NOW.
A sensor returns a CONFIG_ACK packet; gateway emits a config_ack serial JSON
message; Python publishes it to `iot/sensor/<id>/config_ack`.

## Node-RED dashboard
1. Install Node.js LTS.
2. Install Node-RED:
     npm install -g node-red
3. Start:
     node-red
4. Open http://localhost:1880
5. Open Menu > Manage palette > Install and install:
     @flowfuse/node-red-dashboard
6. Drag an `mqtt in` node onto the flow. Configure broker:
     Server: 127.0.0.1
     Port: 1883
     Topic: iot/sensor/1/data
7. Add a JSON node after it (converts MQTT JSON text into an object).
8. Add a Dashboard 2.0 `ui-text` node and set its value to `msg.payload.temperature_c`.
   Put it in a group named Sensor 1.
9. Repeat for `iot/sensor/2/data`, group Sensor 2.
10. Add `mqtt in` nodes for `iot/sensor/1/alarm` and `iot/sensor/2/alarm`;
    connect JSON nodes to `ui-text` widgets showing `msg.payload.alarm`.
11. Add a Dashboard 2.0 `ui-chart` node for each sensor. Feed it a Function node:
      msg.payload = Number(msg.payload.temperature_c);
      msg.topic = "Sensor " + msg.payload.sensor_id;
      return msg;
    (If using the Function node, preserve the original object before replacing payload:
      const d = msg.payload; msg.payload = Number(d.temperature_c);
      msg.topic = "Sensor " + d.sensor_id; return msg;)
12. Add threshold controls. A practical simple arrangement is a `ui-form` with
    fields `sensor_id` and `threshold_c`, followed by a Function node:
      msg.topic = "iot/command/threshold";
      msg.payload = JSON.stringify({
        sensor_id: Number(msg.payload.sensor_id),
        threshold_c: Number(msg.payload.threshold_c)
      });
      return msg;
    Connect to an `mqtt out` node configured to broker 127.0.0.1:1883.
13. Deploy. Open the Dashboard 2.0 page from the dashboard sidebar/link shown by Node-RED.

MQTT broker configuration for Node-RED is `127.0.0.1:1883` when Node-RED
runs on the same laptop as Mosquitto.

## Expected telemetry example
{
  "type":"telemetry",
  "sensor_id":1,
  "sequence":12,
  "temperature_c":31.4,
  "threshold_c":40.0,
  "alarm":false,
  "indicator":0,
  "timestamp_ms":123456
}

## Important design and assignment notes
- The brief requires chip temperature every 5 seconds and a 100 ms LED blink per sample.
- This starter keeps sensors awake initially so commands and alarms can be tested reliably.
  Add and evaluate duty cycling only after the end-to-end baseline works.
- The chip temperature is the ESP32-C3 internal temperature, not ambient room temperature.
- The gateway's alarm-indicator coordination is a basic starting strategy. You must test,
  document and justify selection, reassignment and failure behaviour.
- This package is a starter implementation and has not been compiled or tested on your
  specific boards. Verify compilation, board pinout, MAC address, COM port and channel.
- The brief asks for >=95% observations delivered within 10 seconds, collective alarm
  within 10 seconds, threshold configuration, missing/stale-data identification,
  energy/fairness evaluation, and a 6-8 page report. Collect logs and quantitative
  evidence; do not claim these targets are met until measured.
