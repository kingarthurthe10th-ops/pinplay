"""Serves one CIA to FBI's "Scan QR code" exactly once, then exits.
    python serve_once.py FILE.cia PORT NAME
The file is reachable at http://<host>:PORT/NAME; every other path gets 404. Stops after one complete download."""
import http.server, os, sys, time
FILE, PORT, NAME = sys.argv[1], int(sys.argv[2]), sys.argv[3]
SIZE = os.path.getsize(FILE)
def log(*a): print(time.strftime('%H:%M:%S'), *a, flush=True)
class H(http.server.BaseHTTPRequestHandler):
    done = False
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path != '/' + NAME:
            log('404', self.client_address[0], self.path[:80]); self.send_response(404); self.end_headers(); return
        log('download started by', self.client_address[0])
        self.send_response(200); self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Length', str(SIZE)); self.send_header('Connection', 'close'); self.end_headers()
        sent = 0
        with open(FILE, 'rb') as f:
            while True:
                c = f.read(65536)
                if not c: break
                try: self.wfile.write(c)
                except OSError: log('stopped at', sent, 'of', SIZE); return
                sent += len(c)
        log('sent the whole file (%d bytes)' % SIZE); H.done = True
s = http.server.ThreadingHTTPServer(('0.0.0.0', PORT), H)
s.timeout = 1
log('serving', os.path.basename(FILE), 'at /' + NAME, 'on port', PORT)
while not H.done: s.handle_request()
time.sleep(1); log('closed')
