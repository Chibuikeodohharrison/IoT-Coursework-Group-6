"""
B31OT Serial-to-MQTT bridge.
Laptop USB serial <-> ESP32-C3 gateway <-> ESP-NOW sensors.

Install:
  python -m pip install -r requirements.txt

Run (Windows example):
  python serial_mqtt_bridge.py --port COM5 --broker 127.0.0.1

Use --list-ports to identify the gateway COM port.
"""
import argparse
import json
import logging
import threading
import time

import serial
from serial.tools import list_ports
import paho.mqtt.client as mqtt

BAUD = 115200
TOPIC_PREFIX = "iot"
COMMAND_TOPIC = "iot/command/threshold"

def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        logging.info("Connected to MQTT broker")
        client.subscribe(COMMAND_TOPIC)
        logging.info("Subscribed to %s", COMMAND_TOPIC)
    else:
        logging.error("MQTT connection failed: %s", reason_code)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", help="Gateway serial port, e.g. COM5")
    parser.add_argument("--broker", default="127.0.0.1", help="Mosquitto IP/hostname")
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--baud", type=int, default=BAUD)
    parser.add_argument("--list-ports", action="store_true")
    args = parser.parse_args()

    if args.list_ports:
        for p in list_ports.comports():
            print(f"{p.device}: {p.description}")
        return
    if not args.port:
        parser.error("Specify --port, e.g. --port COM5")

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    ser = serial.Serial(args.port, args.baud, timeout=1, write_timeout=2)
    time.sleep(2)  # many ESP32 boards reset when serial opens
    logging.info("Serial connected: %s at %d", args.port, args.baud)

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="b31ot-serial-bridge")
    client.on_connect = on_connect

    def on_message(mqtt_client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
            sensor_id = int(payload["sensor_id"])
            threshold = float(payload["threshold_c"])
            if sensor_id not in (1, 2):
                raise ValueError("sensor_id must be 1 or 2")
            if not (-40.0 <= threshold <= 125.0):
                raise ValueError("threshold_c outside accepted range")
            command = {
                "cmd": "SET_THRESHOLD",
                "sensor_id": sensor_id,
                "threshold_c": threshold,
            }
            line = json.dumps(command) + "\n"
            ser.write(line.encode("utf-8"))
            logging.info("Forwarded threshold command to gateway: %s", command)
        except Exception as exc:
            logging.exception("Rejected MQTT command: %s", exc)

    client.on_message = on_message
    client.connect(args.broker, args.mqtt_port, 60)
    client.loop_start()

    try:
        logging.info("Serial-MQTT bridge running. Ctrl+C to stop.")
        while True:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if not line.startswith("{"):
                logging.info("Gateway: %s", line)
                continue
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                logging.warning("Non-JSON serial line: %s", line)
                continue

            msg_type = message.get("type", "unknown")
            if msg_type == "telemetry":
                sensor_id = int(message.get("sensor_id", 0))
                if sensor_id not in (1, 2):
                    continue
                topic = f"{TOPIC_PREFIX}/sensor/{sensor_id}/data"
                client.publish(topic, json.dumps(message), qos=1)
                client.publish(f"{TOPIC_PREFIX}/sensor/{sensor_id}/alarm",
                               json.dumps({"sensor_id": sensor_id,
                                           "alarm": message.get("alarm", False),
                                           "timestamp_ms": message.get("timestamp_ms")}), qos=1)
            elif msg_type == "status":
                client.publish(f"{TOPIC_PREFIX}/sensor/{message.get('sensor_id', 0)}/status",
                               json.dumps(message), qos=1)
            elif msg_type == "config_ack":
                client.publish(f"{TOPIC_PREFIX}/sensor/{message.get('sensor_id', 0)}/config_ack",
                               json.dumps(message), qos=1)
            elif msg_type in ("gateway_ready", "error", "command_error", "command_sent"):
                client.publish(f"{TOPIC_PREFIX}/gateway/status", json.dumps(message), qos=1)
            logging.debug("Published %s: %s", msg_type, message)
    except KeyboardInterrupt:
        logging.info("Stopping bridge...")
    finally:
        client.loop_stop()
        client.disconnect()
        ser.close()

if __name__ == "__main__":
    main()
