#!/usr/bin/env python3
"""Clare board-behaviour simulator: replay the ESP32-C6 firmware's exact
network sequence on this machine to isolate board vs backend vs network-path
problems.

Sequence per boot:
  1. Extract the pinned CA chain compiled into clare_ca_chain.h.
  2. POST {base}/api/session                    -> expects {"session_id": ...}
  3. WSS {base}/ws/transcribe/{sid}             (board sends raw PCM binaries)
  4. WSS {base}/ws/host/{sid}                   (board sends Base64 JSON)

Each WebSocket is tried under three trust modes so failures localise fast:
  pinned : verify against kClareCaChainPem exactly like mbedtls on the board
  system : default OS trust store
  none   : no verification (certificate inspectable via raw TLS below)

Also prints the live TLS leaf certificate (issuer/SAN/SHA256) actually served
on this network, which exposes transparent proxies immediately.
"""

import base64
import json
import os
import re
import socket
import ssl
import sys
import time

import requests
import websocket

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
HEADER = os.path.join(PROJECT_ROOT, "03_Clare_C6", "main", "clare_ca_chain.h")
LOCAL = os.path.join(PROJECT_ROOT, "03_Clare_C6", "sdkconfig.local")
PEM_OUT = os.path.join(SCRIPT_DIR, "clare_ca_chain.pem")

TIMEOUT = 20


def read_local(key):
    if not os.path.isfile(LOCAL):
        return None
    m = re.search(r'^%s="(.*)"$' % key, open(LOCAL).read(), re.M)
    return m.group(1) if m else None


def extract_pem():
    src = open(HEADER, encoding="utf-8").read()
    m = re.search(r'R"PEM\((.*?)\)PEM"', src, re.S)
    if not m:
        sys.exit("cannot parse clare_ca_chain.h")
    pem = m.group(1).strip() + "\n"
    os.makedirs(SCRIPT_DIR, exist_ok=True)
    with open(PEM_OUT, "w") as f:
        f.write(pem)
    n = pem.count("BEGIN CERTIFICATE")
    print(f"[pem] extracted {n} certificates -> {PEM_OUT}")
    return PEM_OUT


def http_base():
    base = read_local("CONFIG_CLARE_API_BASE_URL") or ""
    if not base:
        sys.exit("sdkconfig.local missing CONFIG_CLARE_API_BASE_URL")
    return base.rstrip("/")


def show_leaf_cert(host):
    """Raw TCP+TLS peek at whatever this network actually serves."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    try:
        with socket.create_connection((host, 443), timeout=TIMEOUT) as sock:
            t0 = time.time()
            with ctx.wrap_socket(sock, server_hostname=host) as tls:
                dtls = time.time() - t0
                der = tls.getpeercert(binary_form=True)
                import hashlib
                fp = hashlib.sha256(der).hexdigest()
                # decode without cryptography dep
                print(f"[tls] connected={host}:443 handshake={dtls:.3f}s proto={tls.version()} cipher={tls.cipher()[0]}")
                b64 = base64.encodebytes(der).decode()
                tmp = "/tmp/clare_leaf.pem"
                open(tmp, "w").write("-----BEGIN CERTIFICATE-----\n" + b64 + "-----END CERTIFICATE-----\n")
                out = subprocess_run(["openssl", "x509", "-in", tmp, "-noout",
                                      "-subject", "-issuer", "-dates", "-ext", "subjectAltName"])
                print(f"[tls] sha256={fp}")
                print(out.rstrip())
    except Exception as e:
        print(f"[tls] FAILED {type(e).__name__}: {e}")


def subprocess_run(cmd):
    import subprocess
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        return (r.stdout or "") + (r.stderr or "")
    except Exception as e:
        return f"(openssl unavailable: {e})"


def create_session(base):
    url = f"{base}/api/session"
    print(f"[http] POST {url}")
    try:
        r = requests.post(url, json={"topic": read_local("CONFIG_CLARE_TOPIC") or "meeting"},
                          timeout=TIMEOUT)
        print(f"[http] status={r.status_code} bytes={len(r.content)} body={r.text[:120]}")
        sid = r.json().get("session_id") if r.ok else None
        return sid
    except Exception as e:
        print(f"[http] FAILED {type(e).__name__}: {e}")
        return None


def ws_test(url, label, ca_mode, sid):
    """ca_mode: 'pinned' | 'system' | 'none'. Mirrors one board connect."""
    sslopt = {"timeout": TIMEOUT}
    if ca_mode == "pinned":
        sslopt["ca_certs"] = PEM_OUT
    elif ca_mode == "none":
        sslopt["cert_reqs"] = ssl.CERT_NONE
        sslopt["check_hostname"] = False
    print(f"[ws] {label} trust={ca_mode} connecting {url}")
    t0 = time.time()
    try:
        ws = websocket.create_connection(url, timeout=TIMEOUT, sslopt=sslopt,
                                         subprotocols=[], suppress_origin=False)
        dtls = time.time() - t0
        print(f"[ws] CONNECTED in {dtls:.2f}s  tls={ws.sock.version()} cipher={ws.sock.cipher()[0]}")
        try:
            print("[ws] peer served-cert sha256:",
                  __import__("hashlib").sha256(ws.sock.getpeercert(True)).hexdigest())
        except Exception:
            pass
        # Speak just enough protocol to prove the channel works both ways.
        if label == "transcribe":
            pcm = base64.b64encode(b"\x00\x01" * 8)  # tiny dummy, board sends RAW binary though
            ws.send_binary(bytes.fromhex("00010002"))  # raw 16k mono pcm-ish frame
            ws.send(json.dumps({"type": "end"}))
        else:
            ws.send(json.dumps({"type": "audio", "data":
                     base64.b64encode(b"\x00\x01" * 16).decode(), "speaker": "user"}))
            ws.send(json.dumps({"type": "end_of_speech"}))
        deadline = time.time() + 8
        while time.time() < deadline:
            try:
                frame = ws.recv()
                if isinstance(frame, bytes):
                    frame = frame.decode("utf-8", "replace")
                print(f"[ws] recv: {str(frame)[:200]}")
            except websocket.WebSocketTimeoutException:
                break
            except Exception as e:
                print(f"[ws] recv-end {type(e).__name__}: {e}")
                break
        ws.close()
        print(f"[ws] {label}/{ca_mode} RESULT=OK")
        return True
    except Exception as e:
        print(f"[ws] FAILED after {time.time()-t0:.2f}s -> {type(e).__name__}: {e}")
        return False


def main():
    base = http_base()
    host = re.sub(r"^https?://", "", base).split("/")[0]
    print(f"=== Clare WSS simulator  base={base} ===")
    extract_pem()
    show_leaf_cert(host)

    sid = None
    for attempt in range(3):
        sid = create_session(base)
        if sid:
            break
        time.sleep(2)
    if not sid:
        print("\n=== VERDICT: cannot even create session from this machine; "
              "backend/network path broken before any WebSocket matters ===")
        return

    transcribe_url = f"{base.replace('https://', 'wss://')}/ws/transcribe/{sid}"
    host_url = f"{base.replace('https://', 'wss://')}/ws/host/{sid}"
    results = {}
    for mode in ("pinned", "system", "none"):
        results[("transcribe", mode)] = ws_test(transcribe_url, "transcribe", mode, sid)
        results[("host", mode)] = ws_test(host_url, "host", mode, sid)

    print("\n=== SUMMARY ===")
    for k, v in results.items():
        print(f"{k[0]:10s} trust={k[1]:6s} -> {'OK' if v else 'FAIL'}")
    if results[("transcribe", "none")] and not results[("transcribe", "pinned")]:
        print("\nVERDICT: TLS reachable only WITHOUT verification -> "
              "this network's middlebox re-signs traffic; the board's pinned-leaf "
              "trust can never succeed here.")
    elif all(results[(ch, "pinned")] for ch in ("transcribe", "host")):
        print("\nVERDICT: backend fully healthy with BOARD-equivalent trust; "
              "if the board still fails here it is firmware/board-side.")


if __name__ == "__main__":
    main()
