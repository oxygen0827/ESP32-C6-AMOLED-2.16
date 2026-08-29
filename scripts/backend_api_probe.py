#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Local probe for the Clare backend API.
#
# Mimics the same calls the ESP32-C6 firmware makes, but runs on this
# machine.  This lets us separate backend / internet path problems from
# board / Wi-Fi / firmware problems.
#
# Steps:
# 1. POST {base}/api/session  -> create a session
# 2. Open WSS {base}/ws/host/{session_id}
# 3. Send a tiny dummy PCM audio burst + end_of_speech
# 4. Print every WebSocket event received for a few seconds
# 5. Optionally end the session via HTTP
#
# The default base URL is read from 03_Clare_C6/sdkconfig.local so it
# always matches the firmware build config.

import argparse
import base64
import json
import math
import os
import ssl
import sys
import threading
import time

import requests
import websocket

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
SDKCONFIG_LOCAL = os.path.join(PROJECT_ROOT, "03_Clare_C6", "sdkconfig.local")


def read_sdkconfig_base_url(path=SDKCONFIG_LOCAL):
    if not os.path.isfile(path):
        return None
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.startswith("CONFIG_CLARE_API_BASE_URL="):
                val = line.split("=", 1)[1].strip()
                if len(val) >= 2 and val[0] == val[-1] == '"':
                    val = val[1:-1]
                return val
    return None


def generate_pcm_tone(duration_s=0.2, sample_rate=16000, freq=440, amplitude=0.3):
    samples = []
    n = int(duration_s * sample_rate)
    for i in range(n):
        s = amplitude * math.sin(2 * math.pi * freq * i / sample_rate)
        v = int(s * 32760)
        if v > 32767:
            v = 32767
        if v < -32768:
            v = -32768
        samples.append(v)
    return b"".join(v.to_bytes(2, byteorder="little", signed=True) for v in samples)


def make_ws_url(http_base):
    if http_base.startswith("https://"):
        return "wss://" + http_base[8:]
    if http_base.startswith("http://"):
        return "ws://" + http_base[7:]
    return http_base


def build_audio_message(pcm_bytes, speaker="user"):
    return json.dumps(
        {"type": "audio", "data": base64.b64encode(pcm_bytes).decode("ascii"), "speaker": speaker},
        separators=(",", ":"),
    )


class Probe:
    def __init__(self, base_url, topic, ca_file=None, verify=True, timeout=15):
        self.base_url = base_url.rstrip("/")
        self.topic = topic or "meeting"
        self.verify = ca_file if ca_file else verify
        self.timeout = timeout
        self.session_id = None
        self.session_created_at = None

    def http_url(self, path):
        return f"{self.base_url}/{path.lstrip('/')}"

    def create_session(self):
        url = self.http_url("api/session")
        body = {"topic": self.topic}
        print(f"[HTTP] POST {url}\n       body={body}")
        t0 = time.time()
        try:
            r = requests.post(url, json=body, timeout=self.timeout, verify=self.verify)
        except requests.exceptions.SSLError as e:
            print(f"[HTTP] SSL/TLS handshake failed: {e}")
            raise
        except requests.exceptions.ConnectionError as e:
            print(f"[HTTP] connection error: {e}")
            raise
        dt = (time.time() - t0) * 1000
        print(f"[HTTP] status={r.status_code} latency_ms={dt:.1f}\n       response={r.text[:500]}")
        if r.status_code != 200 and r.status_code != 201:
            raise RuntimeError(f"session creation failed: HTTP {r.status_code}")
        data = r.json()
        sid = data.get("session_id")
        if not sid:
            raise RuntimeError(f"session_id missing in response: {data}")
        self.session_id = sid
        self.session_created_at = time.time()
        print(f"[HTTP] session_id={sid}")
        return sid

    def end_session(self):
        if not self.session_id:
            return
        url = self.http_url(f"api/session/{self.session_id}/end")
        print(f"[HTTP] POST {url}")
        try:
            r = requests.post(url, timeout=self.timeout, verify=self.verify)
            print(f"[HTTP] status={r.status_code} response={r.text[:200]}")
        except Exception as e:
            print(f"[HTTP] end-session failed: {e}")

    def probe_host_ws(self, duration_s=10):
        ws_url = f"{make_ws_url(self.base_url)}/ws/host/{self.session_id}"
        print(f"[WS]   connecting {ws_url}")

        sslopt = {}
        if isinstance(self.verify, str):
            sslopt = {"ca_certs": self.verify}
        elif self.verify:
            sslopt = {}
        else:
            sslopt = {"cert_reqs": ssl.CERT_NONE}

        messages = []
        connected = threading.Event()
        closed = threading.Event()

        def on_open(wsapp):
            print("[WS]   connected")
            connected.set()
            pcm = generate_pcm_tone(duration_s=0.2)
            msg = build_audio_message(pcm)
            print(f"[WS]   sending audio chunk ({len(pcm)} bytes PCM -> {len(msg)} JSON bytes)")
            wsapp.send(msg)
            wsapp.send(json.dumps({"type": "end_of_speech"}))

        def on_message(wsapp, msg):
            print(f"[WS]   message: {msg[:500]}{'...' if len(msg) > 500 else ''}")
            messages.append((time.time(), msg))

        def on_error(wsapp, err):
            print(f"[WS]   error: {err}")

        def on_close(wsapp, close_status_code, close_msg):
            print(f"[WS]   closed status={close_status_code} msg={close_msg}")
            closed.set()

        wsapp = websocket.WebSocketApp(
            ws_url,
            on_open=on_open,
            on_message=on_message,
            on_error=on_error,
            on_close=on_close,
        )

        ws_thread = threading.Thread(
            target=wsapp.run_forever, kwargs={"sslopt": sslopt}, daemon=True
        )
        ws_thread.start()

        if not connected.wait(timeout=self.timeout):
            print("[WS]   connect timed out")
            wsapp.close()
            return messages

        closed.wait(timeout=duration_s)
        if not closed.is_set():
            print("[WS]   sending stop and closing")
            try:
                wsapp.send(json.dumps({"type": "stop"}))
            except Exception:
                pass
            wsapp.close()
            closed.wait(timeout=2)

        ws_thread.join(timeout=5)
        return messages


def extract_ca_pem(header_path):
    if not os.path.isfile(header_path):
        return None
    pem_path = os.path.join(SCRIPT_DIR, "clare_ca_chain.pem")
    with open(header_path, "r", encoding="utf-8") as f:
        text = f.read()
    start = text.find('R"PEM(')
    end = text.find(')PEM"', start)
    if start < 0 or end < 0:
        return None
    pem = text[start + 6 : end]
    with open(pem_path, "w", encoding="ascii") as f:
        f.write(pem)
    return pem_path


def main():
    default_base = read_sdkconfig_base_url() or ""

    parser = argparse.ArgumentParser(description="Probe the Clare backend API from this machine")
    parser.add_argument("--base-url", default=default_base,
                        help=f"API base URL (default from sdkconfig.local: {default_base})")
    parser.add_argument("--topic", default="meeting", help="session topic")
    parser.add_argument("--duration", type=float, default=10,
                        help="how many seconds to keep the WebSocket open waiting for answers")
    parser.add_argument("--insecure", action="store_true",
                        help="skip TLS certificate verification")
    parser.add_argument("--use-board-ca", action="store_true",
                        help="use the pinned CA chain from clare_ca_chain.h")
    parser.add_argument("--end-session", action="store_true", default=True,
                        help="call /api/session/{id}/end after the WebSocket test")
    args = parser.parse_args()

    if not args.base_url:
        print("No base URL supplied and none found in 03_Clare_C6/sdkconfig.local")
        sys.exit(1)

    ca_file = None
    if args.use_board_ca:
        ca_file = extract_ca_pem(os.path.join(PROJECT_ROOT, "03_Clare_C6", "main", "clare_ca_chain.h"))
        if ca_file:
            print(f"[CFG]  using pinned CA chain: {ca_file}")

    verify = False if args.insecure else (ca_file if ca_file else True)

    probe = Probe(args.base_url, args.topic, verify=verify)
    try:
        probe.create_session()
        probe.probe_host_ws(duration_s=args.duration)
    except KeyboardInterrupt:
        print("\nInterrupted by user")
    except Exception as e:
        print(f"[ERR]  {type(e).__name__}: {e}")
    finally:
        if args.end_session:
            probe.end_session()


if __name__ == "__main__":
    main()
