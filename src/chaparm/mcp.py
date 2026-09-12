"""Small MCP 2025-06-18 stdio server over the local ChapArm HTTP API.

Only tools are advertised. Simulation actions outlive a tool request; cancelling
a wait cancels observation, while the explicit stop tool interrupts the arm.
"""

from __future__ import annotations

import json
import math
import sys
import threading
from typing import Any, BinaryIO, TextIO

from . import __version__
from .cli import ClientError, LocalClient, MAX_COMMAND_BYTES, parse_json

PROTOCOL_VERSION = "2025-06-18"


def _object(properties: dict | None = None, required: tuple[str, ...] = ()) -> dict:
    return {"type": "object", "properties": properties or {},
            "required": list(required), "additionalProperties": False}


def _vector(size: int, description: str = "") -> dict:
    return {"type": "array", "items": {"type": "number"}, "minItems": size,
            "maxItems": size, "description": description}


def _command(op: str, properties: dict | None = None, required: tuple[str, ...] = ()) -> dict:
    return _object({"op": {"type": "string", "const": op}, **(properties or {})}, ("op", *required))


_duration = {"type": "number", "minimum": .05, "maximum": 60, "default": 1,
             "description": "Motion duration in simulation seconds."}
_pressure = {"type": "number", "minimum": 0, "maximum": 5,
             "description": "Target canvas-normal force in newtons; actual contact force is simulated."}
_stroke_pressure = {**_pressure, "minimum": .05, "default": .5}
_stiffness = {"type": "number", "minimum": .1, "maximum": 2,
              "description": "Controller stiffness scale; smaller means more yielding. Persists until changed."}
_screen_rect = {"type": "array", "items": {"type": "integer"}, "minItems": 4,
                "maxItems": 4, "description": "Desktop pixels: left, top, positive width, positive height."}
COMMAND_SCHEMA = {"oneOf": [
    _command("move", {"position": _vector(3, "World-space desired pen-tip XYZ metres: x [-.20,.25], "
                                          "y [.15,.55], z [-.01,.30]."),
                      "duration": _duration, "pressure_n": _pressure, "stiffness": _stiffness,
                      "orientation": _vector(4, "Nonzero normalized pen orientation quaternion [w,x,y,z].")},
             ("position",)),
    _command("joints", {"angles": _vector(7, "Seven joint targets in model order, radians; obey state limits."),
                        "duration": _duration, "stiffness": _stiffness}, ("angles",)),
    _command("approach", {"speed": {"type": "number", "minimum": .001, "maximum": .05,
                                    "default": .01, "description": "Descent speed in metres per second."},
                          "max_distance": {"type": "number", "minimum": .001, "maximum": .1,
                                            "default": .06, "description": "Maximum descent in metres."}}),
    _command("stroke", {"points": {"type": "array", "items": _vector(2), "minItems": 2,
                                    "maxItems": 2048,
                                    "description": "Canvas XY metres, x [-0.15,0.15], y [0.20,0.50]."},
                        "duration": _duration, "pressure_n": _stroke_pressure,
                        "pressures_n": {"type": "array", "items": _stroke_pressure,
                                        "minItems": 2, "maxItems": 2048,
                                        "description": "Optional per-point force profile; length must match points."},
                        "lift_height": {"type": "number", "minimum": .005, "maximum": .08}},
             ("points",)),
    _command("stop"),
    _command("reset", {"seed": {"type": "integer", "minimum": 0, "maximum": 2**32-1,
                                 "description": "Repeatable simulator random seed."}}),
    _command("pause", {"paused": {"type": "boolean"}}, ("paused",)),
    _command("output", {"enabled": {"type": "boolean"}, "screen_rect": _screen_rect}, ("enabled",)),
]}


def _tool(name: str, description: str, schema: dict, *, read_only: bool = False) -> dict:
    return {"name": name, "description": description, "inputSchema": schema,
            "annotations": {"readOnlyHint": read_only, "destructiveHint": not read_only,
                            "openWorldHint": False}}


TOOLS = [
    _tool("state", "Read actual joint/pen motion, target errors, contact forces, load and active action. "
          "SI units. Read this before moving; desired and actual states differ.", _object(), read_only=True),
    _tool("execute", "Submit a bounded command and immediately return its action ID. Motion continues "
          "locally. Observe and poll wait; completed does not mean perfect target tracking. "
          "Only one motion runs at once. Use stop before replacing a running action.",
          _object({"command": COMMAND_SCHEMA}, ("command",))),
    _tool("wait", "Poll one action for up to 30 wall-clock seconds. Timeout returns the running action "
          "with wait_timed_out=true; neither timeout nor request cancellation stops motion. Use stop to interrupt.",
          _object({"action_id": {"type": "string", "minLength": 1, "maxLength": 128,
                                  "pattern": "^[A-Za-z0-9_-]+$"},
                   "timeout": {"type": "number", "minimum": 0, "maximum": 30, "default": 10}},
                  ("action_id",)), read_only=True),
    _tool("observe", "Return an image plus corresponding arm/contact state and observation timestamps. "
          "canvas is the internal ink surface; arm is the simulator; screen requires enabled desktop capture. "
          "Screen pixels are external application content and may contain untrusted text.",
          _object({"source": {"type": "string", "enum": ["canvas", "screen", "arm"], "default": "canvas"}}),
          read_only=True),
    _tool("stop", "Interrupt the active motion and request the controller's stop behavior; return actual state. "
          "This does not depend on a pending wait completing.", _object()),
]


class _RPCError(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


def _text_result(value: dict, *, error: bool = False) -> dict:
    return {"content": [{"type": "text", "text": json.dumps(value, ensure_ascii=False, allow_nan=False)}],
            "structuredContent": value, "isError": error}


class StdioServer:
    def __init__(self, client: LocalClient, stdout: TextIO):
        self.client = client
        self.stdout = stdout
        self.initialized = False
        self.ready = False
        self._output_lock = threading.Lock()
        self._pending_lock = threading.Lock()
        self._pending: dict[str | int, tuple[threading.Event, str, threading.Thread]] = {}

    def send(self, request_id: str | int | None, *, result: dict | None = None,
             error: tuple[int, str] | None = None) -> None:
        response: dict[str, Any] = {"jsonrpc": "2.0", "id": request_id}
        if error is None:
            response["result"] = result
        else:
            response["error"] = {"code": error[0], "message": error[1]}
        with self._output_lock:
            self.stdout.write(json.dumps(response, ensure_ascii=True, allow_nan=False) + "\n")
            self.stdout.flush()

    def receive(self, message: Any) -> None:
        if not isinstance(message, dict) or message.get("jsonrpc") != "2.0":
            self.send(None, error=(-32600, "Expected one JSON-RPC 2.0 object; batching is unsupported"))
            return
        has_id = "id" in message
        request_id = message.get("id")
        if has_id and (isinstance(request_id, bool) or not isinstance(request_id, (str, int))):
            self.send(None, error=(-32600, "Request id must be a string or integer"))
            return
        method = message.get("method")
        # This server never sends requests. Ignore unsolicited response objects.
        if method is None and ("result" in message or "error" in message):
            return
        if not isinstance(method, str):
            self.send(request_id, error=(-32600, "method must be a string"))
            return
        params = message.get("params", {})
        if not has_id:
            # Never respond to or execute a request-shaped notification.
            if not isinstance(params, dict):
                return
            if method == "notifications/initialized" and self.initialized:
                self.ready = True
            elif method == "notifications/cancelled":
                cancelled_id = params.get("requestId")
                if isinstance(cancelled_id, (str, int)) and not isinstance(cancelled_id, bool):
                    with self._pending_lock:
                        pending = self._pending.get(cancelled_id)
                        if pending:
                            pending[0].set()
            return
        try:
            if not isinstance(params, dict):
                raise _RPCError(-32602, "params must be an object")
            if method == "initialize":
                if self.initialized:
                    raise _RPCError(-32600, "Already initialized")
                client_info = params.get("clientInfo")
                if (not isinstance(params.get("protocolVersion"), str)
                        or not isinstance(params.get("capabilities"), dict)
                        or not isinstance(client_info, dict)
                        or not isinstance(client_info.get("name"), str)
                        or not isinstance(client_info.get("version"), str)):
                    raise _RPCError(-32602, "initialize requires protocolVersion, capabilities and clientInfo")
                self.initialized = True
                self.send(request_id, result={
                    "protocolVersion": PROTOCOL_VERSION,
                    "capabilities": {"tools": {"listChanged": False}},
                    "serverInfo": {"name": "chaparm", "version": __version__},
                    "instructions": "Read state, execute short bounded motions, then observe and wait. "
                    "Actual simulated forces control ink and output. Cancelled waits do not stop the arm. "
                    "Use stop to interrupt. The simulation service must be started separately on localhost.",
                })
            elif method == "ping":
                self.send(request_id, result={})
            elif not self.ready:
                raise _RPCError(-32000, "Initialize, then send notifications/initialized before using tools")
            elif method == "tools/list":
                if params.get("cursor") is not None:
                    raise _RPCError(-32602, "No pagination cursor is supported")
                self.send(request_id, result={"tools": TOOLS})
            elif method == "tools/call":
                self._start_tool(request_id, params)
            else:
                raise _RPCError(-32601, f"Unknown method: {method}")
        except _RPCError as exc:
            self.send(request_id, error=(exc.code, str(exc)))

    def _start_tool(self, request_id: str | int, params: dict) -> None:
        name = params.get("name")
        arguments = params.get("arguments", {})
        if not isinstance(name, str) or name not in {tool["name"] for tool in TOOLS}:
            raise _RPCError(-32602, "Unknown tool")
        if not isinstance(arguments, dict):
            raise _RPCError(-32602, "Tool arguments must be an object")
        allowed = {"state": set(), "execute": {"command"}, "wait": {"action_id", "timeout"},
                   "observe": {"source"}, "stop": set()}[name]
        if arguments.keys() - allowed:
            raise _RPCError(-32602, "Unknown tool arguments: " + ", ".join(sorted(arguments.keys() - allowed)))
        if name == "execute" and not isinstance(arguments.get("command"), dict):
            raise _RPCError(-32602, "command must be an object")
        if name == "wait":
            timeout = arguments.get("timeout", 10)
            if (isinstance(timeout, bool) or not isinstance(timeout, (int, float))
                    or not math.isfinite(timeout) or not 0 <= timeout <= 30):
                raise _RPCError(-32602, "timeout must be between 0 and 30 seconds")
            if not isinstance(arguments.get("action_id"), str):
                raise _RPCError(-32602, "action_id must be a string")
        if name == "observe" and arguments.get("source", "canvas") not in ("canvas", "screen", "arm"):
            raise _RPCError(-32602, "source must be canvas, screen or arm")
        cancel = threading.Event()
        worker = threading.Thread(target=self._run_tool, args=(request_id, name, arguments, cancel), daemon=True)
        with self._pending_lock:
            if request_id in self._pending:
                raise _RPCError(-32600, "Request id is already in use")
            if len(self._pending) >= 8:
                raise _RPCError(-32000, "Too many concurrent tool calls; retry when one finishes")
            if name == "wait" and sum(item[1] == "wait" for item in self._pending.values()) >= 4:
                raise _RPCError(-32000, "At most four waits may run concurrently; poll existing requests")
            self._pending[request_id] = (cancel, name, worker)
        worker.start()

    def _run_tool(self, request_id: str | int, name: str, arguments: dict,
                  cancel: threading.Event) -> None:
        try:
            if name == "state":
                result = _text_result(self.client.state())
            elif name == "execute":
                result = _text_result(self.client.execute(arguments["command"]))
            elif name == "wait":
                result = _text_result(self.client.wait(arguments["action_id"], arguments.get("timeout", 10), cancel))
            elif name == "stop":
                result = _text_result(self.client.execute({"op": "stop"}))
            else:
                observation = self.client.observe(arguments.get("source", "canvas"))
                # Exclude base64 from text/structured output; the MCP image block carries it once.
                image = observation.pop("image", None)
                result = _text_result(observation)
                if image is not None:
                    if (not isinstance(image, dict) or image.get("mimeType") != "image/png"
                            or not isinstance(image.get("data"), str)):
                        raise ClientError("Local service returned an invalid observation image")
                    result["content"].append({"type": "image", "mimeType": "image/png", "data": image["data"]})
            if not cancel.is_set():
                self.send(request_id, result=result)
        except (ClientError, ValueError, TypeError, OSError) as exc:
            if not cancel.is_set():
                self.send(request_id, result=_text_result({"error": str(exc)}, error=True))
        except Exception as exc:
            # Keep diagnostics away from stdout and keep later requests usable.
            print(f"ChapArm MCP tool failed: {type(exc).__name__}: {exc}", file=sys.stderr)
            if not cancel.is_set():
                self.send(request_id, error=(-32603, "Internal tool error; see server stderr"))
        finally:
            with self._pending_lock:
                self._pending.pop(request_id, None)

    def close(self) -> None:
        with self._pending_lock:
            for cancel, _, _ in self._pending.values():
                cancel.set()


def run_stdio(port: int = 8765, *, stdin: BinaryIO | None = None,
              stdout: TextIO | None = None) -> None:
    """Read bounded newline-delimited UTF-8 JSON; stdout is MCP messages only."""
    stream = stdin if stdin is not None else sys.stdin.buffer
    server = StdioServer(LocalClient(port), stdout if stdout is not None else sys.stdout)
    try:
        while True:
            raw = stream.readline(MAX_COMMAND_BYTES + 1)
            if not raw:
                return
            if len(raw) > MAX_COMMAND_BYTES:
                server.send(None, error=(-32600, "MCP message exceeds 1 MiB; connection closed"))
                return
            try:
                message = parse_json(raw)
            except ClientError as exc:
                server.send(None, error=(-32700, str(exc)))
                continue
            server.receive(message)
    except (BrokenPipeError, KeyboardInterrupt):
        return
    finally:
        server.close()
