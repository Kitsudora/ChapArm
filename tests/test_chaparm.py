"""End-to-end behavior checks; no graphics context or Windows driver required."""

import base64
from concurrent.futures import ThreadPoolExecutor
import http.client
from io import BytesIO
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
from unittest.mock import Mock

import numpy as np
from PIL import Image, ImageChops
import pytest

from chaparm.cli import LocalClient, main
from chaparm.mcp import PROTOCOL_VERSION
from chaparm.runtime import BusyError, Runtime
from chaparm.server import LocalServer, MAX_BODY
from chaparm.simulation import JOINT_NAMES, MAX_PRESSURE_N, Simulator
from chaparm.wintab import Publisher


@pytest.fixture
def runtime():
    instance = Runtime(simulator=Simulator(seed=17))
    yield instance
    instance.close()


@pytest.fixture
def http_service(runtime):
    server = LocalServer(runtime, 0)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": .01}, daemon=True)
    thread.start()
    yield server
    server.shutdown()
    server.server_close()
    thread.join(timeout=2)


def advance(runtime, *, seconds=10, action_id=None):
    for _ in range(int(seconds / runtime.sim.timestep) + 1):
        runtime.tick()
        if action_id and runtime.get_action(action_id)["status"] != "running":
            return runtime.get_action(action_id)
    if action_id:
        pytest.fail(f"Action did not finish within {seconds}s: {runtime.state()['action']}")


def request(server, method="POST", body=b'{"op":"stop"}', headers=None):
    connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=3)
    try:
        connection.request(method, "/api/command" if method == "POST" else "/api/state",
                           body=body if method == "POST" else None,
                           headers={"Content-Type": "application/json", **(headers or {})})
        response = connection.getresponse()
        return response.status, json.loads(response.read())
    finally:
        connection.close()


def test_seven_dof_contact_blocks_motion_and_pressure_comes_from_actual_force():
    sim = Simulator(seed=17)
    initial = sim.observe()
    assert sim.model.nq == sim.model.nv == sim.model.nu == 7
    assert initial["joint_names"] == list(JOINT_NAMES)
    sim.set_target([0, .35, 0], pressure_n=.5)
    assert sim.observe()["q"] == initial["q"]
    assert sim.pen_sample()["pressure"] == 0  # A force request in free space is not ink.

    samples = []
    for _ in range(2500):
        sim.step()
        samples.append(sim.observe())
    settled = samples[-500:]
    forces = [state["contact"]["normal_force_n"] for state in settled]
    assert .4 < np.mean(forces) < .65
    assert min(forces) > .1
    # MuJoCo models compliant nib/paper contact, permitting submillimetre compression.
    assert min(state["tip"]["position"][2] for state in samples) > -.0015
    assert max(state["tip"]["position"][2] for state in settled) < .001
    assert np.linalg.norm(settled[-1]["contact_torque"]) > .01
    for state in settled:
        assert state["safety_reason"] is None
        assert state["pen"]["pressure"] == pytest.approx(state["contact"]["normal_force_n"] / MAX_PRESSURE_N)
        assert state["pen"]["contact"]


def test_seeded_imperfection_is_repeatable_and_joint_motion_is_continuous():
    traces = []
    for seed in (7, 7, 8):
        sim = Simulator(seed=seed)
        initial = np.asarray(sim.observe()["q"])
        target = initial + np.array([.10, 0, 0, 0, 0, .10, 0])
        sim.set_joint_target(target.tolist())
        assert np.array_equal(sim.data.qpos, initial)
        trace = [initial]
        for _ in range(400):
            sim.step()
            trace.append(sim.data.qpos.copy())
        traces.append(np.asarray(trace))
    assert np.array_equal(traces[0], traces[1])
    assert np.max(np.abs(traces[0] - traces[2])) > 1e-7
    assert np.max(np.abs(np.diff(traces[0], axis=0))) < .02
    assert np.linalg.norm(traces[0][-1] - traces[0][0]) > .03
    assert np.linalg.norm(traces[0][1] - target) > .05


def test_high_level_stroke_paints_actual_canvas_then_releases(runtime):
    blank = runtime.canvas.image.copy()
    action = runtime.submit({"op": "stroke", "points": [[-.025, .35], [0, .36], [.025, .35]],
                             "duration": 1.5, "pressures_n": [.5, .8, .5]})
    advance(runtime, seconds=.3)
    assert ImageChops.difference(blank, runtime.canvas.image).getbbox() is None
    result = advance(runtime, action_id=action["id"])
    assert result["status"] == "completed"
    assert result["reason"] == "stroke_complete"
    assert result["peak_force_n"] > .1
    changed = ImageChops.difference(blank, runtime.canvas.image).getbbox()
    assert changed is not None and changed[2] - changed[0] > 100
    state, png = runtime.observe_image("canvas")
    assert state["action"]["id"] == action["id"]
    assert state["pen"]["pressure"] == 0 and not state["pen"]["contact"]
    assert state["tip"]["position"][2] > .005
    assert Image.open(BytesIO(png)).size == (1024, 1024)
    kinds = [event["type"] for event in state["events"]]
    assert "contact" in kinds and "released" in kinds and "action_completed" in kinds
    assert all(event["sim_time"] <= state["sim_time"] for event in state["events"])


def test_invalid_commands_and_busy_rejection_preserve_existing_action(runtime):
    action = runtime.submit({"op": "move", "position": [.02, .35, .025], "duration": 1})
    before = runtime.state()
    invalid = [
        {"op": "move", "position": [float("nan"), .35, 0]},
        {"op": "move", "position": [0, .35, 0], "duration": float("inf")},
        {"op": "move", "position": [0, .35, 0], "pressure_n": True},
        {"op": "move", "position": [0, .35, 0], "unknown": 1},
        {"op": "joints", "angles": [0] * 6},
        {"op": "stroke", "points": [[0, .35], [.02, .35]], "pressures_n": [.5]},
        {"op": "reset", "seed": -1},
        {"op": "output", "enabled": True},
    ]
    for command in invalid:
        with pytest.raises(ValueError):
            runtime.submit(command)
        assert runtime.state() == before
    with pytest.raises(BusyError):
        runtime.submit({"op": "approach"})
    assert runtime.state() == before
    result = advance(runtime, action_id=action["id"])
    assert result["status"] == "completed"
    next_action = runtime.submit({"op": "move", "position": [0, .35, .03], "duration": .1})
    assert next_action["id"] != action["id"]
    assert runtime.get_action(action["id"]) == result
    with pytest.raises(KeyError):
        runtime.get_action("0" * 32)

    runtime.submit({"op": "stop"})
    runtime.submit({"op": "move", "position": [-.18, .35, .03], "duration": 1.5})
    advance(runtime, seconds=3)
    assert runtime.state()["tip"]["position"][0] < -.15
    outside = runtime.state()
    with pytest.raises(ValueError, match="canvas"):
        runtime.submit({"op": "move", "position": [0, .35, 0], "pressure_n": .5})
    assert runtime.state() == outside

    runtime.submit({"op": "reset", "seed": 0})
    angles = runtime.state()["q"]
    upper_limit = runtime.state()["joint_limits"][-1][1]
    angles[-1] = upper_limit
    runtime.submit({"op": "joints", "angles": angles, "duration": 1})
    advance(runtime, seconds=4)
    # Soft physical limits allow a tiny overshoot; legal subsequent goals must
    # still be accepted without failing on their first interpolated reference.
    assert runtime.state()["q"][-1] > upper_limit
    repeated = runtime.submit({"op": "joints", "angles": angles, "duration": .1})
    assert advance(runtime, action_id=repeated["id"], seconds=.2)["status"] == "completed"
    assert runtime.state()["fault"] is None


def test_pause_freezes_physics_and_stop_unloads_contact(runtime, monkeypatch):
    action = runtime.submit({"op": "move", "position": [0, .35, 0], "pressure_n": .5, "duration": 10})
    advance(runtime, seconds=4)
    assert runtime.state()["contact"]["normal_force_n"] > .1
    runtime.submit({"op": "pause", "paused": True})
    frozen = runtime.state()
    canvas = runtime.canvas.png()
    advance(runtime, seconds=.1)
    assert runtime.state() == frozen
    assert runtime.canvas.png() == canvas
    runtime.submit({"op": "pause", "paused": False})
    with monkeypatch.context() as patch:
        publisher = Mock(enabled=True)
        publisher.publish.side_effect = RuntimeError("Native bridge disconnected")
        patch.setattr(runtime, "publisher", publisher)
        runtime.submit({"op": "stop"})
        assert not publisher.enabled
        assert any(event["type"] == "output_error" for event in runtime.events)
    assert runtime.get_action(action["id"])["status"] == "cancelled"
    assert runtime.get_action(action["id"])["reason"] == "operator_stop"
    stopped_canvas = runtime.canvas.png()
    advance(runtime, seconds=1.5)
    assert runtime.state()["pen"]["pressure"] == 0
    assert runtime.state()["tip"]["position"][2] > .01
    assert runtime.canvas.png() == stopped_canvas


def test_approach_returns_first_contact_or_bounded_failure(runtime):
    action = runtime.submit({"op": "approach", "speed": .02, "max_distance": .05})
    result = advance(runtime, action_id=action["id"], seconds=4)
    assert result["status"] == "completed" and result["reason"] == "first_contact"
    assert runtime.state()["contact"]["normal_force_n"] > .03
    runtime.submit({"op": "reset"})
    action = runtime.submit({"op": "approach", "speed": .02, "max_distance": .001})
    result = advance(runtime, action_id=action["id"], seconds=.2)
    assert result["status"] == "failed" and result["reason"] == "no_contact_within_distance"


def test_http_rejects_cross_origin_oversize_and_malformed_requests_without_mutation(http_service):
    before = http_service.runtime.state()
    assert request(http_service, headers={"Host": "example.com"})[0] == 403
    assert request(http_service, headers={"Origin": "https://example.com"})[0] == 403
    assert request(http_service, headers={"Content-Type": "text/plain"})[0] == 415
    assert request(http_service, body=b"{}", headers={"Content-Length": str(MAX_BODY + 1)})[0] == 413
    for body in (b'{"op":"move","position":[NaN,0.35,0]}',
                 b'{"op":"move","position":[1e999,0.35,0]}',
                 b'{"op":"stop","extra":1}', b"[" * 1100 + b"]" * 1100,
                 b'{"op":"stop","extra":' + b"[" * 600 + b"0" + b"]" * 600 + b"}",
                 b"not json", b"\xff"):
        assert request(http_service, body=body)[0] == 400
    assert http_service.runtime.state() == before
    status, state = request(http_service, "GET")
    assert status == 200 and state["sim_time"] == before["sim_time"]


def test_http_concurrent_commands_admit_only_one_action(http_service):
    body = b'{"op":"move","position":[0,0.35,0.025],"duration":1}'
    with ThreadPoolExecutor(max_workers=2) as pool:
        responses = list(pool.map(lambda _: request(http_service, body=body), range(2)))
    assert sorted(status for status, _ in responses) == [200, 409]
    accepted = next(result for status, result in responses if status == 200)
    client = LocalClient(http_service.server_port)
    assert client.state()["action"]["id"] == accepted["id"]
    waiting = client.wait(accepted["id"], timeout=0)
    assert waiting["status"] == "running" and waiting["wait_timed_out"]
    client.execute({"op": "stop"})
    assert client.wait(accepted["id"], timeout=0)["status"] == "cancelled"


def test_stdio_mcp_initialization_commands_and_image_observation(http_service):
    environment = {**os.environ, "PYTHONPATH": str(Path(__file__).resolve().parents[1] / "src")}
    process = subprocess.Popen([sys.executable, "-m", "chaparm", "mcp", "--port", str(http_service.server_port)],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, encoding="utf-8", env=environment)
    replies = queue.Queue()
    def read_replies():
        for line in process.stdout:
            replies.put(line)

    reader = threading.Thread(target=read_replies, daemon=True)
    reader.start()

    def send(method, params=None, request_id=None):
        message = {"jsonrpc": "2.0", "method": method, "params": params or {}}
        if request_id is not None:
            message["id"] = request_id
        process.stdin.write(json.dumps(message) + "\n")
        process.stdin.flush()
        if request_id is not None:
            response = json.loads(replies.get(timeout=5))
            assert response["id"] == request_id
            return response

    try:
        response = send("tools/list", request_id=1)
        assert "error" in response
        response = send("initialize", {"protocolVersion": PROTOCOL_VERSION, "capabilities": {},
                                       "clientInfo": {"name": "chaparm-test", "version": "1"}}, 2)
        assert response["result"]["protocolVersion"] == PROTOCOL_VERSION
        send("notifications/initialized")
        tools = send("tools/list", request_id=3)["result"]["tools"]
        assert {tool["name"] for tool in tools} >= {"execute", "observe", "state", "wait", "stop"}
        response = send("tools/call", {"name": "execute", "arguments": {"command": {
            "op": "move", "position": [.02, .35, .025], "duration": 1}}}, 4)
        action = response["result"]["structuredContent"]
        assert action["status"] == "running"
        observation = send("tools/call", {"name": "observe", "arguments": {"source": "canvas"}}, 5)["result"]
        assert observation["structuredContent"]["state"]["action"]["id"] == action["id"]
        image = next(block for block in observation["content"] if block["type"] == "image")
        assert image["mimeType"] == "image/png"
        assert Image.open(BytesIO(base64.b64decode(image["data"], validate=True))).size == (1024, 1024)
        assert "image" not in observation["structuredContent"]
        send("tools/call", {"name": "stop"}, 6)
        assert http_service.runtime.get_action(action["id"])["status"] == "cancelled"
    finally:
        process.stdin.close()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)
        reader.join(timeout=2)
        errors = process.stderr.read()
        process.stdout.close()
        process.stderr.close()
    assert process.returncode == 0, errors
    assert replies.empty()  # stdout contains protocol replies only.


def test_cli_reports_action_json_and_nonfinite_input_errors(http_service, capsys):
    port = str(http_service.server_port)
    assert main(["command", "--port", port, "--json", '{"op":"move","position":[0,0.35,0.025]}']) == 0
    action = json.loads(capsys.readouterr().out)
    assert action["id"] == http_service.runtime.state()["action"]["id"]
    before = http_service.runtime.state()
    assert main(["command", "--port", port, "--json", '{"op":"reset","seed":NaN}']) == 1
    output = capsys.readouterr()
    assert output.out == "" and "Invalid JSON" in json.loads(output.err)["error"]
    assert http_service.runtime.state() == before


@pytest.mark.skipif(sys.platform != "win32", reason="Native ctypes bridge requires Windows")
def test_windows_python_publisher_loads_native_bridge_and_closes_session():
    dll = Path(__file__).resolve().parents[1] / "native/build/x64/Release/Wintab32.dll"
    if not dll.is_file():
        pytest.skip("Build the x64 native provider before exercising the Python bridge")
    publisher = Publisher(dll, session=f"python_test_{os.getpid()}")
    try:
        initial = publisher.status()
        assert initial["publisher_pid"] == os.getpid()
        assert not initial["enabled"] and not initial["ready"]
        publisher.enabled = True
        pen = {"u": .5, "v": .5, "pressure": .25, "contact": True, "proximity": True,
               "tilt_x": 10, "tilt_y": -15, "rotation": 20}
        publisher.publish(pen, (0, 0, 100, 100))
        publisher.publish(pen, (0, 0, 100, 100), active=False)
        assert publisher.status()["published_samples"] == initial["published_samples"] + 2
    finally:
        publisher.close()
    assert publisher.status()["publisher_pid"] == 0


@pytest.mark.skipif(sys.platform != "win32", reason="Krita deployment uses Windows PowerShell")
def test_windows_krita_deployment_preserves_existing_and_changed_files(tmp_path):
    script = Path(__file__).resolve().parents[1] / "scripts/deploy-krita.ps1"
    app_dir = tmp_path / "portable \u4e2d\u6587 \U0001f58c" / "bin"
    app_dir.mkdir(parents=True)

    def pe_fixture(path, machine, payload):
        # Only PE architecture inspection is exercised; these files are never executed.
        data = bytearray(70)
        data[:2] = b"MZ"
        data[60:64] = (64).to_bytes(4, "little")
        data[64:68] = b"PE\0\0"
        data[68:70] = machine.to_bytes(2, "little")
        path.write_bytes(data + payload)
        return path

    app = pe_fixture(app_dir / "krita.exe", 0x8664, b"original application")
    library = pe_fixture(app_dir / "krita.dll", 0x8664, b"original library")
    source_launcher = pe_fixture(tmp_path / "launcher.exe", 0x8664, b"launcher")
    source_dll = pe_fixture(tmp_path / "provider.dll", 0x8664, b"provider")
    wrong_dll = pe_fixture(tmp_path / "wrong.dll", 0x014C, b"wrong architecture")
    original = {path: path.read_bytes() for path in (app, library)}
    launcher = app_dir / "chaparm-krita.exe"
    local_marker = app_dir / "chaparm-krita.exe.local"
    provider = app_dir / "Wintab32.dll"
    manifest = app_dir / "ChapArm.krita-deployment.json"

    def deploy(*, remove=False, dll=source_dll, error=None, env=None):
        args = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", str(script), "-AppExecutable", str(app)]
        args += (["-Remove"] if remove else
                 ["-LauncherPath", str(source_launcher), "-DllPath", str(dll)])
        result = subprocess.run(args, capture_output=True, text=True, encoding="utf-8",
                                errors="replace", timeout=20, env=env)
        output = result.stdout + result.stderr
        if error:
            assert result.returncode != 0 and error in output, output
        else:
            assert result.returncode == 0, output

    deploy(dll=wrong_dll, error="architectures must match")
    deploy(env={**os.environ, "WINDIR": str(tmp_path)}, error="Windows/system directories")
    assert sorted(path.name for path in app_dir.iterdir()) == ["krita.dll", "krita.exe"]
    old_manifest = app_dir / "ChapArm.wintab-deployment.json"
    old_manifest.write_text("{}")
    deploy(error="deploy-wintab.ps1")
    assert old_manifest.read_text() == "{}"
    old_manifest.unlink()
    provider.write_bytes(b"existing tablet provider")
    deploy(error="already exists")
    assert provider.read_bytes() == b"existing tablet provider"
    provider.unlink()
    deploy()
    assert launcher.read_bytes() == source_launcher.read_bytes()
    assert provider.read_bytes() == source_dll.read_bytes()
    assert local_marker.is_file() and local_marker.read_bytes() == b""
    record = json.loads(manifest.read_text(encoding="utf-8-sig"))
    assert record["application"].casefold() == str(app).casefold()
    deployed = {path: path.read_bytes() for path in (launcher, local_marker, provider, manifest)}
    deploy(error="already exists")
    for changed in (launcher, local_marker, provider):
        changed.write_bytes(deployed[changed] + b"changed")
        deploy(remove=True, error="A deployed file changed")
        for untouched in deployed.keys() - {changed}:
            assert untouched.read_bytes() == deployed[untouched]
        changed.write_bytes(deployed[changed])
    deploy(remove=True)
    assert sorted(path.name for path in app_dir.iterdir()) == ["krita.dll", "krita.exe"]
    for path, contents in original.items():
        assert path.read_bytes() == contents
