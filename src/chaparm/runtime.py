"""Persistent action execution. Only the local loop advances physical time."""

from collections import deque
import copy
import math
import threading
import time
import uuid

import numpy as np

from .observations import Canvas, arm_png, screen_png, png_bytes
from .simulation import Simulator
from .wintab import Publisher, validate_rect


class BusyError(ValueError):
    pass


def number(value, name, low, high):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f"{name} must be a finite number")
    if not low <= value <= high:
        raise ValueError(f"{name} must be in [{low}, {high}]")
    return float(value)


def vector(value, size, name, low=-1, high=1):
    if not isinstance(value, (list, tuple)) or len(value) != size:
        raise ValueError(f"{name} must have {size} numbers")
    return [number(x, name, low, high) for x in value]


def smooth(t):
    t = max(0, min(1, t))
    return t*t*t*(10 + t*(-15 + 6*t))


class Runtime:
    def __init__(self, *, simulator=None, wintab_dll=None, session="default", screen_rect=None, capture=False):
        self.lock = threading.RLock()
        self.sim = simulator or Simulator()
        self.canvas = Canvas()
        self.screen_rect = validate_rect(screen_rect) if screen_rect is not None else None
        self.publisher = Publisher(wintab_dll, session) if wintab_dll else None
        self.capture_enabled = capture
        self.paused = False
        self.fault = None
        self.action = None
        self._work = None
        self.history = deque(maxlen=128)
        self.events = deque(maxlen=128)
        self.force_trace = deque(maxlen=120)
        self._last_contact = False
        self._output_suspended = True
        self._steps = 0
        self._shutdown = threading.Event()
        self._thread = None
        self.deadline_misses = 0
        self.epoch = 0
        self._event_sequence = 0

    def start(self):
        self._thread = threading.Thread(target=self._run, name="chaparm-physics", daemon=True)
        self._thread.start()

    def close(self):
        self._shutdown.set()
        if self._thread:
            self._thread.join(timeout=5)
        with self.lock:
            if self.publisher:
                self.publisher.close()

    def _run(self):
        dt = float(self.sim.model.opt.timestep)
        deadline = time.perf_counter()
        while not self._shutdown.is_set():
            deadline += dt
            try:
                self.tick()
            except Exception as exc:
                with self.lock:
                    self.fault = str(exc)
                    self.paused = True
                    self._output_suspended = True
                    self._finish("failed", "simulation_error")
                    if self.publisher:
                        self.publisher.enabled = False
            remaining = deadline - time.perf_counter()
            if remaining < -.05:
                self.deadline_misses += 1
                deadline = time.perf_counter()
            elif remaining > 0:
                self._shutdown.wait(remaining)

    def _event(self, kind, **values):
        self._event_sequence += 1
        self.events.append({"type": kind, "sim_time": float(self.sim.data.time), "epoch": self.epoch,
                            "sequence": self._event_sequence,
                            "action_id": self.action["id"] if self.action else None, **values})

    def _finish(self, status="completed", reason=None):
        if self.action and self.action["status"] == "running":
            self.action.update(status=status, reason=reason, ended_at=float(self.sim.data.time))
            self._event("action_" + status, reason=reason)
            self.history.append(copy.deepcopy(self.action))
        self._work = None

    def _unload(self):
        self.sim.hold(unload=True)
        self._output_suspended = True
        self.canvas.previous = None
        self._release_output()

    def _release_output(self):
        if self.publisher:
            try:
                self.publisher.publish(self.sim.pen_sample(), self.screen_rect, active=False)
            except (RuntimeError, OSError) as exc:
                self.publisher.enabled = False
                self._event("output_error", message=str(exc))

    def _validate(self, raw):
        if not isinstance(raw, dict) or not isinstance(raw.get("op"), str):
            raise ValueError("command must be an object with an op string")
        op = raw["op"]
        fields = {
            "move": {"position", "duration", "pressure_n", "stiffness", "orientation"},
            "joints": {"angles", "duration", "stiffness"},
            "approach": {"speed", "max_distance"},
            "stroke": {"points", "duration", "pressure_n", "pressures_n", "lift_height"},
            "stop": set(), "reset": {"seed"}, "pause": {"paused"},
            "output": {"enabled", "screen_rect"},
        }
        if op not in fields:
            raise ValueError(f"unknown op: {op}")
        extra = set(raw) - fields[op] - {"op"}
        if extra:
            raise ValueError("unknown fields: " + ", ".join(sorted(extra)))
        try:
            c = copy.deepcopy(raw)
        except RecursionError as exc:
            raise ValueError("command nesting is too deep") from exc
        if op in ("move", "joints", "stroke"):
            c["duration"] = number(c.get("duration", 1), "duration", .05, 60)
        if "pressure_n" in c:
            c["pressure_n"] = number(c["pressure_n"], "pressure_n", 0, 5)
        if "stiffness" in c:
            c["stiffness"] = number(c["stiffness"], "stiffness", .1, 2)
        if "orientation" in c:
            c["orientation"] = vector(c["orientation"], 4, "orientation", -1, 1)
            if np.linalg.norm(c["orientation"]) < .001:
                raise ValueError("orientation quaternion cannot be zero")
        if op == "move":
            c["position"] = vector(c.get("position"), 3, "position", -.8, .8)
            self.sim.validate_target(c["position"], pressure_n=c.get("pressure_n"),
                                     orientation=c.get("orientation"), stiffness=c.get("stiffness"))
            if c.get("pressure_n", 0) > 0:
                p = self.sim.observe()["tip"]["position"]
                if not (-.15 <= p[0] <= .15 and .20 <= p[1] <= .50):
                    raise ValueError("pressure move requires a tip over the canvas; move to a hover position first")
        elif op == "joints":
            c["angles"] = vector(c.get("angles"), 7, "angles", -math.pi, math.pi)
            limits = self.sim.model.jnt_range[:7]
            if np.any(np.array(c["angles"]) < limits[:, 0]) or np.any(np.array(c["angles"]) > limits[:, 1]):
                raise ValueError("angles exceed the model joint limits; inspect state.joint_limits")
        elif op == "approach":
            c["speed"] = number(c.get("speed", .01), "speed", .001, .05)
            c["max_distance"] = number(c.get("max_distance", .06), "max_distance", .001, .10)
            p = self.sim.observe()["tip"]["position"]
            if not (-.15 <= p[0] <= .15 and .20 <= p[1] <= .50 and -.01 <= p[2] <= .30):
                raise ValueError("approach requires a tip over the canvas; move to a hover position first")
        elif op == "stroke":
            points = c.get("points")
            if not isinstance(points, list) or not 2 <= len(points) <= 2048:
                raise ValueError("stroke requires 2..2048 points")
            c["points"] = [vector(p, 2, "point", -.5, .5) for p in points]
            if any(not (-.15 <= x <= .15 and .20 <= y <= .50) for x, y in c["points"]):
                raise ValueError("stroke points must lie on canvas x[-.15,.15], y[.20,.50]")
            if np.linalg.norm(np.diff(c["points"], axis=0), axis=1).sum() < 1e-6:
                raise ValueError("stroke must have nonzero length")
            c["pressure_n"] = number(c.get("pressure_n", .5), "pressure_n", .05, 5)
            c["lift_height"] = number(c.get("lift_height", .02), "lift_height", .005, .08)
            if "pressures_n" in c:
                values = c["pressures_n"]
                if not isinstance(values, list) or len(values) != len(points):
                    raise ValueError("pressures_n must match points in length")
                c["pressures_n"] = [number(v, "pressures_n", .05, 5) for v in values]
        elif op == "pause":
            if not isinstance(c.get("paused"), bool):
                raise ValueError("paused must be boolean")
        elif op == "output":
            if not isinstance(c.get("enabled"), bool):
                raise ValueError("enabled must be boolean")
            if "screen_rect" in c:
                c["screen_rect"] = validate_rect(c["screen_rect"])
            if c["enabled"] and (self.publisher is None or (c.get("screen_rect") or self.screen_rect) is None):
                raise ValueError("output needs --wintab-dll and a calibrated screen_rect")
        elif op == "reset" and "seed" in c:
            seed = c["seed"]
            if isinstance(seed, bool) or not isinstance(seed, int) or not 0 <= seed <= 2**32-1:
                raise ValueError("seed must be an unsigned 32-bit integer")
        return c

    def submit(self, command):
        with self.lock:
            c = self._validate(command)
            op = c["op"]
            if op == "stop":
                self._finish("cancelled", "operator_stop")
                self._unload()
                return {"status": "completed", "op": op, "action": copy.deepcopy(self.action)}
            if op == "reset":
                self._finish("cancelled", "reset")
                self._unload()
                self.sim.reset(**({"seed": c["seed"]} if "seed" in c else {}))
                self.epoch += 1
                self.canvas.clear()
                self.paused, self.fault = False, None
                self._last_contact = False
                self.force_trace.clear()
                self._event("reset")
                return {"status": "completed", "op": op}
            if op == "pause":
                self.paused = c["paused"]
                if self.paused:
                    self.canvas.previous = None
                    self._release_output()
                self._event("paused" if self.paused else "resumed")
                return {"status": "completed", "op": op, "paused": self.paused}
            if op == "output":
                if c.get("screen_rect"):
                    self.screen_rect = c["screen_rect"]
                if self.publisher:
                    self.publisher.enabled = c["enabled"]
                    if not c["enabled"]:
                        self._release_output()
                return {"status": "completed", "op": op, "enabled": c["enabled"]}
            if self.fault:
                raise BusyError("Simulator faulted; inspect state and reset")
            if self.paused:
                raise BusyError("Simulator is paused; resume before submitting an action")
            if self._work is not None:
                raise BusyError("An action is running; wait or stop it before submitting another")
            state = self.sim.observe()
            self.action = {"id": uuid.uuid4().hex, "op": op, "status": "running", "stage": op,
                           "started_at": state["sim_time"], "epoch": self.epoch, "progress": 0., "peak_force_n": 0.}
            # A prior joint-space action may leave the Cartesian command workspace.
            # The core still rate-limits the reference; do not send invalid intermediate goals.
            initial_reference = np.clip(state["tip"]["position"], [-.20, .15, -.01], [.25, .55, .30])
            limits = self.sim.model.jnt_range[:7]
            self._work = {"command": c, "start": state["sim_time"], "position": initial_reference,
                          "angles": np.clip(state["q"], limits[:, 0], limits[:, 1]),
                          "stage_start": state["sim_time"], "stage": "position"}
            if op == "stroke":
                distances = np.r_[0., np.cumsum(np.linalg.norm(np.diff(c["points"], axis=0), axis=1))]
                self._work["path_t"] = distances / distances[-1]
            self._output_suspended = False
            self._event("action_started")
            return copy.deepcopy(self.action)

    def _advance_action(self):
        w = self._work
        if w is None:
            return
        c = w["command"]
        elapsed = float(self.sim.data.time) - w["start"]
        op = c["op"]
        if op in ("move", "joints"):
            fraction = min(1., elapsed / c["duration"])
            alpha = smooth(fraction)
            self.action["progress"] = fraction
            if op == "move":
                p = w["position"] * (1-alpha) + np.array(c["position"]) * alpha
                self.sim.set_target(p.tolist(), pressure_n=c.get("pressure_n"),
                                    stiffness=c.get("stiffness"), orientation=c.get("orientation"))
            else:
                limits = self.sim.model.jnt_range[:7]
                q = np.clip(w["angles"] + (np.array(c["angles"])-w["angles"])*alpha,
                            limits[:, 0], limits[:, 1])
                self.sim.set_joint_target(q.tolist(), stiffness=c.get("stiffness"))
            if fraction >= 1:
                self._finish(reason="reference_complete")
        elif op == "approach":
            max_distance = min(c["max_distance"], max(0., w["position"][2]+.0095))
            distance = min(max_distance, elapsed*c["speed"])
            self.action["progress"] = distance/max(max_distance, 1e-9)
            p = w["position"].copy()
            p[2] -= distance
            self.sim.set_target(p.tolist())
            if distance >= max_distance:
                self._finish("failed", "no_contact_within_distance")
                self._unload()
        elif op == "stroke":
            stage_time = float(self.sim.data.time) - w["stage_start"]
            self.action["stage"] = w["stage"]
            if w["stage"] == "position":
                p = np.array([*c["points"][0], c["lift_height"]])
                alpha = smooth(stage_time)
                self.sim.set_target(((1-alpha)*w["position"] + alpha*p).tolist())
                if stage_time >= 1.2:
                    w.update(stage="contact", stage_start=float(self.sim.data.time))
            elif w["stage"] == "contact":
                self.sim.set_target([*c["points"][0], 0.], pressure_n=c.get("pressures_n", [c["pressure_n"]])[0])
                if stage_time > 6:
                    self._finish("failed", "contact_timeout")
                    self._unload()
            elif w["stage"] == "draw":
                fraction = min(1., stage_time/c["duration"])
                t = smooth(fraction)
                points = np.asarray(c["points"])
                p = [float(np.interp(t, w["path_t"], points[:, j])) for j in (0, 1)]
                force = float(np.interp(t, w["path_t"], c["pressures_n"])) if "pressures_n" in c else c["pressure_n"]
                self.sim.set_target([*p, 0.], pressure_n=force)
                self.action["progress"] = fraction
                if fraction >= 1:
                    w.update(stage="lift", stage_start=float(self.sim.data.time),
                             lift_start=np.array(self.sim.observe()["tip"]["position"]))
            elif w["stage"] == "lift":
                alpha = smooth(stage_time/.6)
                end = np.array([*c["points"][-1], c["lift_height"]])
                self.sim.set_target(((1-alpha)*w["lift_start"] + alpha*end).tolist())
                if stage_time >= .6:
                    self._finish(reason="stroke_complete")

    def tick(self):
        with self.lock:
            if self.paused:
                return
            self._advance_action()
            self.sim.step()
            pen = self.sim.pen_sample()
            state = self.sim.observe()
            force = state["contact"]["normal_force_n"]
            if pen["contact"] != self._last_contact:
                self._event("contact" if pen["contact"] else "released", normal_force_n=force)
                self._last_contact = pen["contact"]
            if self.action and self.action["status"] == "running":
                self.action["peak_force_n"] = max(force, self.action["peak_force_n"])
            if self._work and (state.get("safety_reason") == "contact_force_limit" or force > 8):
                self._finish("failed", "force_limit")
                self._unload()
            if self._work and force > .03:
                if self._work["command"]["op"] == "approach":
                    self.sim.hold()
                    self._finish(reason="first_contact")
                elif self._work["command"]["op"] == "stroke" and self._work["stage"] == "contact":
                    self._work.update(stage="draw", stage_start=float(self.sim.data.time))
            self.canvas.update(pen, not self._output_suspended)
            self._steps += 1
            if self._steps % 10 == 0:
                self.force_trace.append([state["sim_time"], force])
            if self.publisher and self._steps % 2 == 0:
                try:
                    self.publisher.publish(pen, self.screen_rect, active=not self._output_suspended)
                except RuntimeError as exc:
                    self._event("output_error", message=str(exc))

    def state(self):
        with self.lock:
            state = self.sim.observe()
            state.update(action=copy.deepcopy(self.action), paused=self.paused, running=not self.paused,
                         fault=self.fault, events=list(self.events), force_trace=list(self.force_trace),
                         joint_limits=self.sim.model.jnt_range[:7].tolist(),
                         screen_rect=self.screen_rect, capture_enabled=self.capture_enabled,
                         epoch=self.epoch, frame_id=self._steps,
                         deadline_misses=self.deadline_misses,
                         wintab=self.publisher.status() if self.publisher else {"available": False, "enabled": False})
            return state

    def get_action(self, action_id):
        with self.lock:
            if self.action and self.action["id"] == action_id:
                return copy.deepcopy(self.action)
            for action in reversed(self.history):
                if action["id"] == action_id:
                    return copy.deepcopy(action)
        raise KeyError("Unknown or expired action ID")

    def image(self, source):
        return self.observe_image(source)[1]

    def observe_image(self, source):
        with self.lock:
            state = self.state()
            state["observed_at_unix"] = time.time()
            if source == "canvas":
                image = self.canvas.image.copy()
            elif source == "arm":
                image = None
            elif source == "screen":
                if not self.capture_enabled:
                    raise ValueError("Screen capture is disabled; restart with --capture")
                rect = self.screen_rect
            else:
                raise ValueError("source must be canvas, arm or screen")
        # PNG encoding and OS capture must never hold the dynamics lock.
        if source == "canvas":
            encoded = png_bytes(image)
        elif source == "arm":
            encoded = arm_png(state)
        else:
            encoded = screen_png(rect)
            state["screen_captured_at_unix"] = time.time()
        return state, encoded
