#!/usr/bin/env python3
"""Opt-in diagnosis: independently observe the SDK publisher using Paho/TCP.

Requires paho-mqtt. Both connections use a unique, non-retained test topic.
A failed SDK run remains a failure even if the observer misses the same packets.
"""
import argparse
import json
import os
import re
import subprocess
import threading
import time
from urllib.parse import urlparse

import paho.mqtt.client as mqtt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", help="mqtt_quic_broker_test executable")
    parser.add_argument("--url", required=True, help="MQTT over QUIC URL")
    parser.add_argument("--tcp-host", help="same broker over TCP; defaults to URL host")
    parser.add_argument("--tcp-port", type=int, default=1883)
    parser.add_argument("--rate", type=int, default=10)
    parser.add_argument("--messages", type=int, default=100, help="messages per QoS")
    parser.add_argument("--version", choices=[4, 5], type=int, default=5)
    args = parser.parse_args()
    if not 1 <= args.rate <= 100 or not 1 <= args.messages <= 3000:
        parser.error("rate must be 1..100 and messages must be 1..3000")
    total = args.messages * 2
    ready = threading.Event()
    seen, duplicates, errors = {}, [], []
    topic = "nanosdk/quic-regression/observed-%d-%d" % (os.getpid(), time.time_ns())
    kwargs = dict(client_id="nanosdk-observer-%d" % os.getpid(), protocol=mqtt.MQTTv311)
    if hasattr(mqtt, "CallbackAPIVersion"):
        kwargs["callback_api_version"] = mqtt.CallbackAPIVersion.VERSION2
    client = mqtt.Client(**kwargs)

    def connected(c, userdata, flags, reason, properties=None):
        if reason != 0:
            errors.append("observer CONNECT rejected: %s" % reason)
            ready.set()
        else:
            c.subscribe(topic, 2)

    def subscribed(c, userdata, mid, granted, properties=None):
        if any((code.is_failure if hasattr(code, "is_failure") else code >= 128) for code in granted):
            errors.append("observer SUBSCRIBE rejected")
        ready.set()

    def received(c, userdata, message):
        # The harness also sends an idle/UNSUBSCRIBE probe after a successful
        # rate phase. Only rate messages have this sequence/payload layout.
        if len(message.payload) != 1024:
            return
        seq = int.from_bytes(message.payload[:4], "big")
        if seq >= total:
            return
        expected = bytes((seq * 17 + j) & 255 for j in range(4, 1024))
        if message.payload[4:] != expected:
            errors.append("payload mismatch at %d" % seq)
        if seq in seen:
            duplicates.append(seq)
        seen[seq] = message.qos

    client.on_connect = connected
    client.on_subscribe = subscribed
    client.on_message = received
    client.connect(args.tcp_host or urlparse(args.url).hostname, args.tcp_port, 60)
    client.loop_start()
    try:
        if not ready.wait(15) or errors:
            raise RuntimeError("observer subscription failed: %s" % errors)
        env = dict(os.environ, NNG_BROKER_TEST_TOPIC=topic,
                   NNG_BROKER_TEST_RATE_MESSAGES=str(args.messages),
                   NNG_BROKER_TEST_RATE=str(args.rate))
        command = [args.executable, args.url, str(args.version), "0", "1"]
        result = subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=total / args.rate + 90)
        print(result.stdout, end="", flush=True)
        time.sleep(1)
    finally:
        client.unsubscribe(topic)
        client.disconnect()
        client.loop_stop()
    missing = [i for i in range(total) if i not in seen]
    matches = re.findall(r"missing:([ 0-9]*)", result.stdout)
    sdk_missing = [int(x) for x in matches[-1].split()] if matches else []
    print(json.dumps(dict(observer="independent Paho TCP", rate=args.rate,
                          received=len(seen), missing=missing, sdk_missing=sdk_missing,
                          same_missing=missing == sdk_missing, duplicates=duplicates,
                          errors=errors, sdk_exit=result.returncode)), flush=True)
    return 1 if result.returncode or missing or duplicates or errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
