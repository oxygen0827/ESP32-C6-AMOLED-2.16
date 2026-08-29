#!/usr/bin/env python3
"""Minimal HTTPS+WSS test server for board-side WSS stall isolation.

Endpoints:
  POST /voice-api/api/session          -> {"session_id": "localtest123"}
  GET  /voice-api/ws/transcribe/<id>   -> complete the WS upgrade, hold, log frames
  GET  /voice-api/ws/host/<id>         -> same
  GET  /voice-api/health               -> 200 ok
Everything is logged with timestamps so we can see exactly how far the
board's connection gets (TCP, TLS, upgrade request, frames).
"""
import http.server
import json
import ssl
import sys
import time
import base64
import hashlib
import os

PORT = 8443
LOG = "/tmp/local_ws_server.log"


def log(msg):
    line = f"{time.strftime('%H:%M:%S')} {msg}"
    print(line, flush=True)
    with open(LOG, "a") as f:
        f.write(line + "\n")


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        log(f"{self.client_address[0]}:{self.client_address[1]} {fmt % args}")

    def _send_json(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        log(f"POST {self.path} from {self.client_address}")
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            self.rfile.read(length)
        if self.path.endswith("/api/session"):
            self._send_json({"session_id": "localtest123"})
        elif self.path.endswith("/end"):
            self._send_json({"status": "ended"})
        else:
            self._send_json({"ok": True})

    def do_GET(self):
        log(f"GET {self.path} upgrade={self.headers.get('Upgrade')}")
        if self.headers.get("Upgrade", "").lower() == "websocket":
            key = self.headers.get("Sec-WebSocket-Key", "")
            accept = base64.b64encode(
                hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
            ).decode()
            self.send_response(101, "Switching Protocols")
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", accept)
            self.end_headers()
            log("WS upgrade accepted; holding connection 30s")
            self.close_connection = False
            deadline = time.time() + 30
            self.connection.settimeout(5)
            while time.time() < deadline:
                try:
                    data = self.connection.recv(4096)
                    if not data:
                        log("WS peer closed")
                        break
                    log(f"WS frame bytes: {len(data)} first={data[:16].hex()}")
                except Exception:
                    pass
            return
        if self.path.endswith("/health"):
            self._send_json({"status": "ok"})
        else:
            self._send_json({"ok": True})


def main():
    cert = "/tmp/local_ws_cert.pem"
    key = "/tmp/local_ws_key.pem"
    if not (os.path.exists(cert) and os.path.exists(key)):
        import subprocess
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048",
            "-keyout", key, "-out", cert, "-days", "3", "-nodes",
            "-subj", "/CN=192.168.236.200",
            "-addext", "subjectAltName=IP:192.168.236.200",
        ], check=True)
    server = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    server.socket = ctx.wrap_socket(server.socket, server_side=True)
    log(f"listening on 0.0.0.0:{PORT} (TLS)")
    server.serve_forever()


if __name__ == "__main__":
    main()
