#!/usr/bin/env python3
"""Minimal SSE fixture server. Usage: sse_fixture_server.py [port]"""
import http.server
import sys
import time


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"  # no chunked framing needed

    def do_GET(self):
        if self.path != "/stream":
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        last = self.headers.get("Last-Event-ID", "")
        n = int(last) if last.isdigit() else 0
        try:
            while True:
                n += 1
                self.wfile.write(f"id: {n}\ndata: tick {n}\n\n".encode())
                self.wfile.flush()
                if n % 5 == 0:
                    self.wfile.write(b": keepalive\n")
                    self.wfile.flush()
                time.sleep(1)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *a):
        pass


port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
