#!/usr/bin/env python3
"""Opt-in UDP fault proxy for mqtt_quic_broker_test; no host firewall changes.

Usage: python3 quic_broker_fault_test.py TEST_BINARY [--version 4|5]
Only forwards to broker.emqx.io:14567. The C harness uses unique, nonretained
MQTT topics. This is not an offline CTest and needs explicit network access.
"""
import argparse
import heapq
import queue
import select
import socket
import subprocess
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary")
    parser.add_argument("--version", choices=("4", "5"), default="5")
    args = parser.parse_args()
    target = (socket.gethostbyname("broker.emqx.io"), 14567)
    proxy = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    proxy.bind(("127.0.0.1", 0))
    upstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    upstream.connect(target)
    stop = threading.Event()
    faults = queue.Queue()
    counters = {"packets": 0, "dropped": 0, "delayed": 0, "blackouts": 0}
    errors = []

    def relay():
        client = None
        blocked_until = 0
        delayed = []
        try:
            while not stop.is_set():
                now = time.monotonic()
                try:
                    faults.get_nowait()
                    blocked_until = now + 7
                    delayed.clear()
                    counters["blackouts"] += 1
                except queue.Empty:
                    pass
                while delayed and delayed[0][0] <= now:
                    _, _, data, destination = heapq.heappop(delayed)
                    if destination == target:
                        upstream.send(data)
                    else:
                        proxy.sendto(data, destination)
                ready, _, _ = select.select([proxy, upstream], [], [], 0.01)
                if not ready:
                    continue
                source = ready[0]
                data, sender = source.recvfrom(65535)
                if source is upstream:
                    if client is None:
                        continue
                    destination = client
                else:
                    client = sender
                    destination = target
                counters["packets"] += 1
                index = counters["packets"]
                # Deterministic loss and delay/reordering throughout the run.
                if now < blocked_until or index % 19 == 0:
                    counters["dropped"] += 1
                elif index % 13 == 0:
                    heapq.heappush(delayed, (now + 0.04, index, data, destination))
                    counters["delayed"] += 1
                elif destination == target:
                    upstream.send(data)
                else:
                    proxy.sendto(data, destination)
        except Exception as exc:
            errors.append(exc)

    worker = threading.Thread(target=relay)
    worker.start()
    process = None
    timer = None
    try:
        url = f"mqtt-quic://127.0.0.1:{proxy.getsockname()[1]}"
        process = subprocess.Popen(
            [args.binary, url, args.version, "1", "3", "--network-fault"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        timer = threading.Timer(180, process.kill)
        timer.start()
        for line in process.stdout:
            print(line, end="", flush=True)
            if line.strip() == "FAULT blackhole":
                faults.put(True)
        result = process.wait()
        print(f"UDP faults: {counters}; child exit={result}", flush=True)
        if errors:
            raise errors[0]
        return result if result else int(counters["blackouts"] != 1)
    finally:
        if timer:
            timer.cancel()
        if process and process.poll() is None:
            process.kill()
            process.wait()
        stop.set()
        worker.join()
        proxy.close()
        upstream.close()


if __name__ == "__main__":
    raise SystemExit(main())
