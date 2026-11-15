"""Checks PinPlay's join steps from a PC (stdlib only, mirrors source/kahoot.c):

    python tools/kprobe.py PIN

Reserves the session, solves the join puzzle, opens the game's websocket and does the CometD handshake,
then disconnects. It never logs in, so no player is added to the game.
"""
import base64, json, os, re, socket, ssl, struct, sys, time

UA = "Mozilla/5.0 (Nintendo 3DS; Mobile; rv:140.0) Gecko/20100101 Firefox/140.0"


def tls(host):
    s = socket.create_connection((host, 443), timeout=15)
    return ssl.create_default_context().wrap_socket(s, server_hostname=host)


def http_get(host, path):
    s = tls(host)
    s.sendall(f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUser-Agent: {UA}\r\nAccept: */*\r\n"
              f"Origin: https://kahoot.it\r\nReferer: https://kahoot.it/\r\nConnection: close\r\n\r\n".encode())
    data = b""
    while True:
        c = s.recv(65536)
        if not c: break
        data += c
    head, _, body = data.partition(b"\r\n\r\n")
    lines = head.decode("latin1").split("\r\n")
    status = int(lines[0].split()[1])
    hdr = {l.split(":", 1)[0].strip().lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
    if hdr.get("transfer-encoding", "").lower() == "chunked":
        out, rest = b"", body
        while rest:
            n, _, rest = rest.partition(b"\r\n")
            n = int(n.split(b";")[0], 16)
            if n == 0: break
            out += rest[:n]; rest = rest[n + 2:]
        body = out
    return status, hdr, body


def solve(challenge, token_b64):
    # Same parsing as kahoot.it's own extractMessageAndOffset: message = first quoted word,
    # offset = text between the first '=' and the next ';' (pure integer arithmetic).
    msg = re.search(r"'(\d*[a-z]*[A-Z]*)\w+'", challenge).group(0)[1:-1]
    eq = challenge[challenge.index("=") + 1:]
    eq = eq[:eq.index(";")].strip()
    offset = calc(eq)
    key = "".join(chr((ord(c) * i + offset) % 77 + 48) for i, c in enumerate(msg))
    tok = base64.b64decode(token_b64)
    return "".join(chr(b ^ ord(key[i % len(key)])) for i, b in enumerate(tok))


def calc(expr):
    """Tiny recursive-descent evaluator for + - * / ( ) integers, like the C one."""
    toks = re.findall(r"\d+|[-+*/()]", expr)
    if "".join(toks) != re.sub(r"\s", "", expr): raise ValueError("bad offset: " + expr)
    pos = [0]
    def peek(): return toks[pos[0]] if pos[0] < len(toks) else None
    def take(): pos[0] += 1; return toks[pos[0] - 1]
    def atom():
        t = take()
        if t == "(": v = add(); take(); return v
        if t == "-": return -atom()
        if t == "+": return atom()
        return int(t)
    def mul():
        v = atom()
        while peek() in ("*", "/"):
            op = take(); r = atom()
            v = v * r if op == "*" else int(v / r)
        return v
    def add():
        v = mul()
        while peek() in ("+", "-"):
            op = take(); r = mul()
            v = v + r if op == "+" else v - r
        return v
    return add()


class WS:
    def __init__(self, host, path):
        self.s = tls(host)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nOrigin: https://kahoot.it\r\n"
                       f"User-Agent: {UA}\r\n\r\n".encode())
        buf = b""
        while b"\r\n\r\n" not in buf: buf += self.s.recv(4096)
        head, _, self.buf = buf.partition(b"\r\n\r\n")
        if b" 101 " not in head.split(b"\r\n")[0]: raise RuntimeError(head.decode("latin1"))

    def send(self, obj):
        data = json.dumps(obj, separators=(",", ":")).encode()
        print(">>", data.decode()[:300])
        mask = os.urandom(4)
        n = len(data)
        hdr = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
        self.s.sendall(hdr + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(data)))

    def _need(self, n):
        while len(self.buf) < n:
            c = self.s.recv(65536)
            if not c: raise EOFError
            self.buf += c
        r, self.buf = self.buf[:n], self.buf[n:]
        return r

    def recv(self):
        msg = b""
        while True:
            b0, b1 = self._need(2)
            n = b1 & 127
            if n == 126: n = struct.unpack(">H", self._need(2))[0]
            elif n == 127: n = struct.unpack(">Q", self._need(8))[0]
            payload = self._need(n)
            op = b0 & 15
            if op == 8: raise EOFError("close " + payload[:2].hex())
            if op == 9: continue  # ping (server pings are rare; ignore)
            msg += payload
            if b0 & 0x80: return msg.decode()


def main():
    pin = sys.argv[1]
    st, hdr, body = http_get("kahoot.it", f"/reserve/session/{pin}/?{int(time.time()*1000)}")
    print("reserve", st, {k: v for k, v in hdr.items() if k.startswith("x-kahoot")})
    if st != 200: print(body[:200]); return
    res = json.loads(body)
    print("reserve body keys:", {k: (v if k != "challenge" else "...") for k, v in res.items()})
    print("challenge:", res["challenge"])
    sid = solve(res["challenge"], hdr["x-kahoot-session-token"])
    print("session:", sid)
    ws = WS("kahoot.it", f"/cometd/{pin}/{sid}")
    now = lambda: int(time.time() * 1000)
    ws.send([{"id": "1", "version": "1.0", "minimumVersion": "1.0", "channel": "/meta/handshake",
              "supportedConnectionTypes": ["websocket", "long-polling", "callback-polling"],
              "advice": {"timeout": 60000, "interval": 0}, "ext": {"ack": True, "timesync": {"tc": now(), "l": 0, "o": 0}}}])
    while True:
        for m in json.loads(ws.recv()):
            print("<<", json.dumps(m)[:300])
            if m.get("channel") == "/meta/handshake":
                if not m.get("successful"): print("handshake failed"); return
                ws.send([{"id": "2", "channel": "/meta/connect", "connectionType": "websocket", "advice": {"timeout": 0},
                          "clientId": m["clientId"], "ext": {"ack": 0, "timesync": {"tc": now(), "l": 0, "o": 0}}}])
            elif m.get("channel") == "/meta/connect":
                print("CHECK OK" if m.get("successful") else "connect failed"); return


if __name__ == "__main__":
    main()
