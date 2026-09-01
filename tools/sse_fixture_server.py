#!/usr/bin/env python3
"""SSE fixture server for manual smoke tests and the POSIX integration suite.

Usage: sse_fixture_server.py [port]

Listens on `port` (default 8080) and `port + 1` (a second origin, used as a
cross-origin redirect target). Paths on the primary port:

  /stream       SSE stream: id/data ticks, keepalive comments, honors
                Last-Event-ID for resume.
  /silent       Completes headers, stays silent for 3 s, then streams.
  /early        Sends a "103 Early Hints" block before the real 200 stream.
  /redirect     302 with a relative Location to /stream (same origin).
  /bigredirect  302 to /stream with a 40 KiB body (exceeds the curl port's
                32 KiB ring).
  /xorigin      302 to the second port (cross origin; must NOT be followed).
  /badtarget    302 to /closenow, which closes without any response.
  /204          204 No Content (server-requested permanent stop).
"""
import socketserver
import sys
import threading
import time


def sse_ticks(w, tag, last_event_id):
    n = int(last_event_id) if last_event_id.isdigit() else 0
    while True:
        n += 1
        w.write(f"id: {n}\ndata: {tag} {n}\n\n".encode())
        w.flush()
        if n % 5 == 0:
            w.write(b": keepalive\n")
            w.flush()
        time.sleep(0.3)


def sse_headers(w):
    w.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            b"Connection: close\r\n\r\n")
    w.flush()


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        req = self.rfile.readline().decode(errors="replace")
        parts = req.split(" ")
        path = parts[1] if len(parts) > 1 else "/"
        last_event_id = ""
        while True:
            line = self.rfile.readline()
            if not line or line in (b"\r\n", b"\n"):
                break
            text = line.decode(errors="replace").rstrip()
            if text.lower().startswith("last-event-id:"):
                last_event_id = text.split(":", 1)[1].strip()
        w = self.wfile
        port = self.server.server_address[1]
        xorigin_port = self.server.xorigin_port
        try:
            if path == "/stream":
                sse_headers(w)
                sse_ticks(w, f"tick{port}", last_event_id)
            elif path == "/silent":
                sse_headers(w)
                time.sleep(3)
                sse_ticks(w, "late", last_event_id)
            elif path == "/early":
                w.write(b"HTTP/1.1 103 Early Hints\r\n"
                        b"Link: </s>; rel=preload\r\n\r\n")
                w.flush()
                time.sleep(0.2)
                sse_headers(w)
                sse_ticks(w, "early", last_event_id)
            elif path == "/redirect":
                w.write(b"HTTP/1.1 302 Found\r\nLocation: /stream\r\n"
                        b"Content-Length: 0\r\nConnection: close\r\n\r\n")
            elif path == "/bigredirect":
                body = b"x" * (40 * 1024)
                w.write(b"HTTP/1.1 302 Found\r\nLocation: /stream\r\n"
                        b"Content-Type: text/html\r\n"
                        + f"Content-Length: {len(body)}\r\n".encode()
                        + b"Connection: close\r\n\r\n")
                w.write(body)
                w.flush()
            elif path == "/xorigin":
                w.write(f"HTTP/1.1 302 Found\r\n"
                        f"Location: http://127.0.0.1:{xorigin_port}/stream\r\n"
                        f"Content-Length: 0\r\nConnection: close\r\n\r\n"
                        .encode())
            elif path == "/badtarget":
                w.write(b"HTTP/1.1 302 Found\r\nLocation: /closenow\r\n"
                        b"Content-Length: 0\r\nConnection: close\r\n\r\n")
            elif path == "/closenow":
                pass  # close without writing any response
            elif path == "/204":
                w.write(b"HTTP/1.1 204 No Content\r\n"
                        b"Connection: close\r\n\r\n")
            else:
                w.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                        b"Connection: close\r\n\r\n")
        except (BrokenPipeError, ConnectionResetError):
            pass


class Srv(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    primary = Srv(("127.0.0.1", port), Handler)
    secondary = Srv(("127.0.0.1", port + 1), Handler)
    primary.xorigin_port = port + 1
    secondary.xorigin_port = port + 1
    threading.Thread(target=secondary.serve_forever, daemon=True).start()
    primary.serve_forever()


if __name__ == "__main__":
    main()
