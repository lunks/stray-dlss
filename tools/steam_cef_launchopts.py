#!/usr/bin/env python3
"""Set a Steam app's launch options through steamwebhelper's CEF remote debugger (port 8080).

Usage: steam_cef_launchopts.py <appid> <launch options string>
Stdlib-only WebSocket client: finds the SharedJSContext target and evaluates
SteamClient.Apps.SetAppLaunchOptions(appid, opts).
"""
import base64, hashlib, json, os, socket, struct, sys, urllib.request

appid, opts = int(sys.argv[1]), sys.argv[2]

targets = json.load(urllib.request.urlopen("http://localhost:8080/json", timeout=5))
tgt = next(t for t in targets if t.get("title") == "SharedJSContext")
ws_url = tgt["webSocketDebuggerUrl"]  # ws://localhost:8080/devtools/page/<id>
path = ws_url.split("localhost:8080", 1)[1]

s = socket.create_connection(("localhost", 8080), timeout=10)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall((f"GET {path} HTTP/1.1\r\nHost: localhost:8080\r\nUpgrade: websocket\r\n"
           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
           f"Sec-WebSocket-Version: 13\r\n\r\n").encode())
resp = b""
while b"\r\n\r\n" not in resp:
    resp += s.recv(4096)
assert b"101" in resp.split(b"\r\n", 1)[0], resp[:200]

def send_text(payload: bytes):
    mask = os.urandom(4)
    n = len(payload)
    if n < 126:      head = struct.pack("!BB", 0x81, 0x80 | n)
    elif n < 65536:  head = struct.pack("!BBH", 0x81, 0x80 | 126, n)
    else:            head = struct.pack("!BBQ", 0x81, 0x80 | 127, n)
    s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

buf = b""
def recv_exact(n):
    global buf
    while len(buf) < n:
        chunk = s.recv(65536)
        if not chunk: raise EOFError
        buf += chunk
    out, buf = buf[:n], buf[n:]
    return out

def recv_text():
    while True:
        b0, b1 = recv_exact(2)
        ln = b1 & 0x7F
        if ln == 126: ln = struct.unpack("!H", recv_exact(2))[0]
        elif ln == 127: ln = struct.unpack("!Q", recv_exact(8))[0]
        payload = recv_exact(ln)
        if (b0 & 0x0F) == 1:
            return payload

js = f"SteamClient.Apps.SetAppLaunchOptions({appid}, {json.dumps(opts)})"
send_text(json.dumps({"id": 1, "method": "Runtime.evaluate",
                      "params": {"expression": js, "awaitPromise": True,
                                 "returnByValue": True}}).encode())
while True:
    msg = json.loads(recv_text())
    if msg.get("id") == 1:
        print(json.dumps(msg.get("result", msg), indent=1)[:500])
        break
s.close()
