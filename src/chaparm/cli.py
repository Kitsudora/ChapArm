"""Command-line entry points and the shared loopback HTTP client."""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

MAX_COMMAND_BYTES = 1024 * 1024
MAX_RESPONSE_BYTES = 32 * 1024 * 1024


class ClientError(Exception):
    """An actionable command or local service error."""


def _reject_constant(value: str) -> None:
    raise ValueError(f"Nonfinite JSON number: {value}")


def parse_json(value: str | bytes) -> Any:
    """Read strict JSON; Python's nonstandard NaN/Infinity inputs are rejected."""
    try:
        result = json.loads(value, parse_constant=_reject_constant)
        # This also rejects exponent overflow such as 1e999 and excessive depth.
        json.dumps(result, allow_nan=False)
        return result
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise ClientError(f"Invalid JSON: {exc}") from exc


def _port(value: str | int) -> int:
    try:
        port = int(value)
    except (ValueError, TypeError) as exc:
        raise argparse.ArgumentTypeError("Port must be an integer") from exc
    if isinstance(value, bool) or not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("Port must be between 1 and 65535")
    return port


class LocalClient:
    """Bounded requests to the local simulation; no environment HTTP proxy."""

    def __init__(self, port: int = 8765):
        self.base_url = f"http://127.0.0.1:{_port(port)}"
        self._opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def request(self, path: str, command: dict | None = None, *,
                timeout: float = 10, binary: bool = False) -> Any:
        data = None
        headers = {"Accept": "image/png" if binary else "application/json"}
        if command is not None:
            if not isinstance(command, dict):
                raise ClientError("Command must be a JSON object")
            try:
                data = json.dumps(command, allow_nan=False).encode("utf-8")
            except (ValueError, TypeError, RecursionError) as exc:
                raise ClientError(f"Invalid command: {exc}") from exc
            if len(data) > MAX_COMMAND_BYTES:
                raise ClientError("Command exceeds 1 MiB")
            headers["Content-Type"] = "application/json"
        request = urllib.request.Request(self.base_url + path, data=data, headers=headers)
        try:
            with self._opener.open(request, timeout=timeout) as response:
                payload = response.read(MAX_RESPONSE_BYTES + 1)
        except urllib.error.HTTPError as exc:
            body = exc.read(MAX_COMMAND_BYTES)
            try:
                detail = parse_json(body)
                message = detail.get("error", str(detail)) if isinstance(detail, dict) else str(detail)
            except ClientError:
                message = f"Local service returned HTTP {exc.code}"
            raise ClientError(message) from exc
        except (urllib.error.URLError, OSError) as exc:
            raise ClientError(f"Cannot reach ChapArm at {self.base_url}: {exc}") from exc
        if len(payload) > MAX_RESPONSE_BYTES:
            raise ClientError("Local service response exceeds 32 MiB")
        if binary:
            if not payload.startswith(b"\x89PNG\r\n\x1a\n"):
                raise ClientError("Local service did not return a PNG image")
            return payload
        result = parse_json(payload)
        if not isinstance(result, dict):
            raise ClientError("Local service returned a non-object response")
        return result

    def state(self) -> dict:
        return self.request("/api/state")

    def execute(self, command: dict) -> dict:
        return self.request("/api/command", command)

    def wait(self, action_id: str, timeout: float = 30,
             cancel: threading.Event | None = None) -> dict:
        if not isinstance(action_id, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", action_id):
            raise ClientError("action_id must contain 1–128 letters, digits, underscores or hyphens")
        if (isinstance(timeout, bool) or not isinstance(timeout, (int, float))
                or not math.isfinite(timeout) or not 0 <= timeout <= 300):
            raise ClientError("timeout must be a finite number between 0 and 300 seconds")
        deadline = time.monotonic() + timeout
        while True:
            if cancel is not None and cancel.is_set():
                raise ClientError("Wait cancelled; the simulation action continues. Use stop to end it.")
            action = self.request(f"/api/actions/{action_id}", timeout=min(5, max(.1, deadline - time.monotonic())))
            if action.get("status") in {"completed", "cancelled", "failed"}:
                return action
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return {**action, "wait_timed_out": True}
            if cancel is None:
                time.sleep(min(.1, remaining))
            else:
                cancel.wait(min(.1, remaining))

    def observe(self, source: str = "canvas") -> dict:
        if source not in {"canvas", "screen", "arm"}:
            raise ClientError("source must be canvas, screen or arm")
        return self.request("/api/observe?" + urllib.parse.urlencode({"source": source}))

    def image(self, source: str = "canvas") -> bytes:
        paths = {"canvas": "/api/canvas.png", "screen": "/api/capture.png", "arm": "/api/arm.png"}
        if source not in paths:
            raise ClientError("source must be canvas, screen or arm")
        return self.request(paths[source], binary=True)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="ChapArm seven-DoF arm and force-aware virtual pen")
    sub = parser.add_subparsers(dest="subcommand", required=True)
    serve = sub.add_parser("serve", help="Run the persistent local simulation and HTTP service")
    serve.add_argument("--viewer", action="store_true", help="Open the native MuJoCo viewer")
    serve.add_argument("--wintab-dll", type=Path, help="Path to the application-scoped ChapArm Wintab DLL")
    serve.add_argument("--session", default="default", help="Native virtual pen session name")
    serve.add_argument("--screen-rect", type=int, nargs=4, metavar=("LEFT", "TOP", "WIDTH", "HEIGHT"),
                       help="Desktop pixel rectangle mapped to the virtual drawing surface")
    serve.add_argument("--capture", action="store_true", help="Enable optional Windows screen capture")
    state = sub.add_parser("state", help="Get actual arm, pen, contact and action state as JSON")
    command = sub.add_parser("command", help="Submit an action or runtime command as JSON")
    group = command.add_mutually_exclusive_group(required=True)
    group.add_argument("--file", type=Path, help="Read a UTF-8 JSON command from a file")
    group.add_argument("--json", help="Inline JSON command; see docs/agent-guide.md")
    wait = sub.add_parser("wait", help="Poll an action until terminal status or timeout")
    wait.add_argument("action_id")
    wait.add_argument("--timeout", type=float, default=60, help="Maximum wait in seconds (0–300)")
    observe = sub.add_parser("observe", help="Save a current observation as a PNG")
    observe.add_argument("--out", type=Path, required=True)
    observe.add_argument("--source", choices=("canvas", "screen", "arm"), default="canvas")
    mcp = sub.add_parser("mcp", help="Serve agent tools over MCP stdio; connect to an existing simulation")
    for command_parser in (serve, state, command, wait, observe, mcp):
        command_parser.add_argument("--port", type=_port, default=8765, help="Loopback service port (default: 8765)")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.subcommand == "serve":
            from .server import serve
            serve(port=args.port, viewer=args.viewer, wintab_dll=args.wintab_dll,
                  session=args.session, screen_rect=args.screen_rect, capture=args.capture)
            return 0
        if args.subcommand == "mcp":
            from .mcp import run_stdio
            run_stdio(port=args.port)
            return 0
        client = LocalClient(args.port)
        if args.subcommand == "state":
            result = client.state()
        elif args.subcommand == "command":
            if args.file:
                with args.file.open("rb") as stream:
                    data = stream.read(MAX_COMMAND_BYTES + 1)
            else:
                data = args.json.encode("utf-8")
            if len(data) > MAX_COMMAND_BYTES:
                raise ClientError("Command exceeds 1 MiB")
            result = client.execute(parse_json(data))
        elif args.subcommand == "wait":
            result = client.wait(args.action_id, args.timeout)
        else:
            image = client.image(args.source)
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_bytes(image)
            result = {"path": str(args.out.resolve()), "source": args.source}
        print(json.dumps(result, ensure_ascii=False, allow_nan=False))
        return 0
    except KeyboardInterrupt:
        return 130
    except (ClientError, OSError, ValueError, RuntimeError) as exc:
        print(json.dumps({"error": str(exc)}, ensure_ascii=False), file=sys.stderr)
        return 1
