"""Loopback-only HTTP host for the operator UI and agent clients."""

import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from importlib.resources import files
import json
import mimetypes
import re
import sys
import time
from urllib.parse import parse_qs, urlsplit

from .runtime import BusyError, Runtime

MAX_BODY = 131072


def strict_json(data):
    def invalid(value):
        raise ValueError(f"Nonfinite JSON number: {value}")
    try:
        return json.loads(data, parse_constant=invalid)
    except RecursionError as exc:
        raise ValueError("JSON nesting is too deep") from exc


class LocalServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, runtime, port):
        self.runtime = runtime
        super().__init__(("127.0.0.1", port), Handler)


class Handler(BaseHTTPRequestHandler):
    server_version = "ChapArm/0.1"

    def setup(self):
        super().setup()
        self.connection.settimeout(5)

    def log_message(self, fmt, *args):
        if len(args) > 1 and str(args[1]).startswith(("4", "5")):
            super().log_message(fmt, *args)

    def _allowed(self):
        port = self.server.server_port
        hosts = {f"127.0.0.1:{port}", f"localhost:{port}"}
        if self.headers.get("Host") not in hosts:
            self._json({"error": "Invalid Host"}, 403)
            return False
        origin = self.headers.get("Origin")
        if origin and origin not in {f"http://{h}" for h in hosts}:
            self._json({"error": "Cross-origin access is not allowed"}, 403)
            return False
        return True

    def _send(self, body, content_type, status=200):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; frame-ancestors 'none'")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _json(self, value, status=200):
        self._send(json.dumps(value, allow_nan=False, separators=(",", ":")).encode("utf-8"),
                   "application/json; charset=utf-8", status)

    def do_GET(self):
        if not self._allowed():
            return
        url = urlsplit(self.path)
        path = url.path
        runtime = self.server.runtime
        try:
            if path == "/api/state":
                self._json(runtime.state())
            elif path.startswith("/api/actions/"):
                action_id = path.removeprefix("/api/actions/")
                if not re.fullmatch(r"[a-f0-9]{32}", action_id):
                    raise ValueError("Invalid action ID")
                self._json(runtime.get_action(action_id))
            elif path in ("/api/canvas.png", "/api/arm.png", "/api/capture.png"):
                source = {"/api/canvas.png": "canvas", "/api/arm.png": "arm", "/api/capture.png": "screen"}[path]
                self._send(runtime.image(source), "image/png")
            elif path == "/api/observe":
                query = parse_qs(url.query)
                source = query.get("source", ["canvas"])[0]
                state, image = runtime.observe_image(source)
                self._json({"source": source, "state": state,
                            "image": {"mimeType": "image/png", "data": base64.b64encode(image).decode("ascii")}})
            else:
                name = "index.html" if path == "/" else path.removeprefix("/")
                if "/" in name or ".." in name or not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
                    raise KeyError("Not found")
                resource = files("chaparm").joinpath("web", name)
                if not resource.is_file():
                    raise KeyError("Not found")
                self._send(resource.read_bytes(), mimetypes.guess_type(name)[0] or "application/octet-stream")
        except KeyError as exc:
            self._json({"error": str(exc)}, 404)
        except (ValueError, RuntimeError, OSError) as exc:
            self._json({"error": str(exc)}, 400)

    def do_POST(self):
        if not self._allowed():
            return
        if self.path != "/api/command":
            self._json({"error": "Not found"}, 404)
            return
        if self.headers.get_content_type() != "application/json":
            self._json({"error": "Content-Type must be application/json"}, 415)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= MAX_BODY:
                self._json({"error": f"Body must be 1..{MAX_BODY} bytes"}, 413)
                self.close_connection = True
                return
            body = self.rfile.read(length)
            if len(body) != length:
                raise ValueError("Incomplete request body")
            self._json(self.server.runtime.submit(strict_json(body)))
        except BusyError as exc:
            self._json({"error": str(exc)}, 409)
        except (ValueError, TypeError, RuntimeError, OSError) as exc:
            self._json({"error": str(exc)}, 400)


def serve(*, port=8765, viewer=False, wintab_dll=None, session="default", screen_rect=None, capture=False):
    if sys.platform == "win32":
        import ctypes
        # Coordinates and capture rectangles are physical virtual-desktop pixels.
        user32 = ctypes.WinDLL("user32")
        try:
            user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        except AttributeError:
            user32.SetProcessDPIAware()
    runtime = Runtime(wintab_dll=wintab_dll, session=session, screen_rect=screen_rect, capture=capture)
    server = LocalServer(runtime, port)
    runtime.start()
    print(f"ChapArm listening on http://127.0.0.1:{server.server_port}", file=sys.stderr)
    try:
        if viewer:
            import threading
            import mujoco.viewer
            http = threading.Thread(target=server.serve_forever, daemon=True)
            http.start()
            with mujoco.viewer.launch_passive(runtime.sim.model, runtime.sim.data) as window:
                while window.is_running():
                    with runtime.lock:
                        window.sync()
                    time.sleep(1/60)
            server.shutdown()
        else:
            server.serve_forever(poll_interval=.2)
    except KeyboardInterrupt:
        pass
    finally:
        runtime.close()
        server.server_close()
