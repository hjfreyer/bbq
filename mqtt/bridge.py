#!/usr/bin/env python3

import os.path
import sys
import pathlib
from google.cloud import firestore
from typing import Any
import paho.mqtt.client as mqtt
import logging

logging.basicConfig(stream=sys.stdout, encoding="utf-8", level=logging.INFO)


MQTT_USER = "bbq-bridge"
MQTT_PASSWORD = os.environ.get("MQTT_PASSWORD")


# The callback for when the client receives a CONNACK response from the server.
def on_connect(client, userdata, flags, reason_code, properties):
    logging.info(f"Connected with result code {reason_code}")

    # Subscribing in on_connect() means that if we lose the connection and
    # reconnect then subscriptions will be renewed.
    client.subscribe("/bbq/#")


def on_message(client: Any, db: firestore.Client, msg: mqtt.MQTTMessage) -> None:
    topic = pathlib.Path(msg.topic)
    signal = topic.name
    session = topic.parent.name
    prefix = topic.parent.parent.name

    if prefix != "bbq":
        logging.error("Bad topic name: %s", msg.topic)
        return
    db.document("sessions", session).set({"last_update": firestore.SERVER_TIMESTAMP})
    db.collection("sessions", session, signal).add(
        {"timestamp": firestore.SERVER_TIMESTAMP, "value": float(msg.payload)}
    )


def main() -> None:
    db = firestore.Client(project="hjfreyer-bbq")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.user_data_set(db)
    client.on_connect = on_connect
    client.on_message = on_message
    client.enable_logger()

    client.tls_set()
    client.username_pw_set(os.environ["MQTT_USER"], os.environ["MQTT_PASSWORD"])
    client.connect(os.environ["MQTT_HOST"], 8883, 60)
    client.loop_forever()


if __name__ == "__main__":
    main()
