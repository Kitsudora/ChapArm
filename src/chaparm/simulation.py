"""Torque-driven seven-DoF arm with a rigid grip and force-producing nib contact.

World axes: x right, y away from the shoulder, z up. Distances are metres;
quaternions use [w, x, y, z]. This module never changes qpos during a step.
Inverse kinematics is used only to construct the initial, unloaded posture.
"""

from __future__ import annotations

from importlib.resources import files
import math
from numbers import Real

import mujoco
import numpy as np


JOINT_NAMES = (
    "shoulder_yaw", "shoulder_pitch", "shoulder_roll", "elbow_flexion",
    "forearm_rotation", "wrist_flexion", "wrist_deviation",
)
CANVAS_BOUNDS = {"x": [-0.15, 0.15], "y": [0.20, 0.50], "z": 0.0}
MAX_PRESSURE_N = 5.0
CONTACT_LIMIT_N = 8.0
NIB_RADIUS = 0.0015


def _number(value, name: str, low: float, high: float) -> float:
    if isinstance(value, bool) or not isinstance(value, Real):
        raise ValueError(f"{name} must be a number")
    value = float(value)
    if not math.isfinite(value) or not low <= value <= high:
        raise ValueError(f"{name} must be finite and in [{low}, {high}]")
    return value


def _vector(value, length: int, name: str) -> np.ndarray:
    if not isinstance(value, (list, tuple, np.ndarray)) or np.ndim(value) != 1 or len(value) != length:
        raise ValueError(f"{name} must contain {length} numbers")
    try:
        result = np.array([_number(v, name, -1e6, 1e6) for v in value], dtype=float)
    except (TypeError, OverflowError) as exc:
        raise ValueError(f"{name} must contain finite numbers") from exc
    return result


def _quaternion(matrix: np.ndarray) -> np.ndarray:
    quat = np.empty(4)
    mujoco.mju_mat2Quat(quat, np.asarray(matrix).reshape(9))
    if quat[0] < 0:
        quat *= -1
    return quat


def _matrix(quat: np.ndarray) -> np.ndarray:
    result = np.empty(9)
    mujoco.mju_quat2Mat(result, quat)
    return result.reshape(3, 3)


def _rotation_error(target: np.ndarray, actual: np.ndarray) -> np.ndarray:
    quat = _quaternion(target @ actual.T)
    length = np.linalg.norm(quat[1:])
    if length < 1e-10:
        return 2 * quat[1:]
    return quat[1:] * (2 * math.atan2(length, quat[0]) / length)


class Simulator:
    """A single-owner simulation; the service serializes calls with its own lock.

    ``pressure_n=None`` selects Cartesian impedance control. A positive pressure
    selects normal-force control against z=0, with the requested x/y trajectory.
    ``pressure_n=0`` selects an unloaded hover at least 5 mm above the canvas.
    Actual force is always measured from MuJoCo contact, never from the command.
    """

    def __init__(self, seed: int = 0, timestep: float = 1 / 500):
        timestep = _number(timestep, "timestep", 0.0005, 0.005)
        xml = files("chaparm").joinpath("assets/right_arm.xml").read_text(encoding="utf-8")
        self.model = mujoco.MjModel.from_xml_string(xml)
        self.model.opt.timestep = timestep
        self.data = mujoco.MjData(self.model)
        self._tip_id = self.model.site("nib_center").id
        self._nib_id = self.model.geom("pen_nib").id
        self._canvas_id = self.model.geom("canvas").id
        self._pen_body_id = self.model.body("pen").id
        self._jacp = np.zeros((3, 7))
        self._jacr = np.zeros((3, 7))
        self._limits = self.model.jnt_range.copy()
        self._joint_kp = np.array([45., 45., 24., 22., 6., 5., 4.])
        self._joint_kd = np.array([8., 8., 4., 3., 0.6, 0.5, 0.4])
        self._torque_limits = self.model.actuator_ctrlrange[:, 1].copy()
        self._seed = self._validate_seed(seed)
        self.reset()

    @staticmethod
    def _validate_seed(seed) -> int:
        if isinstance(seed, bool) or not isinstance(seed, (int, np.integer)) or not 0 <= seed < 2**32:
            raise ValueError("seed must be an integer in [0, 2**32)")
        return int(seed)

    @property
    def timestep(self) -> float:
        return float(self.model.opt.timestep)

    def _tip(self, data=None) -> np.ndarray:
        # The spherical nib's lowest point, independent of shaft orientation.
        data = self.data if data is None else data
        return data.site_xpos[self._tip_id] - np.array([0., 0., NIB_RADIUS])

    def _orientation(self, data=None) -> np.ndarray:
        data = self.data if data is None else data
        return data.site_xmat[self._tip_id].reshape(3, 3)

    def _initial_posture(self) -> np.ndarray:
        scratch = mujoco.MjData(self.model)
        scratch.qpos[:] = [-0.2, -1.1, 0.7, 1.6, -0.7, -0.7, 0.2]
        desired = np.array([0., 0.35, 0.035])
        desired_rot = _matrix(np.array([math.cos(0.12), -math.sin(0.12), 0., 0.]))
        jp, jr = np.zeros((3, 7)), np.zeros((3, 7))
        for _ in range(300):
            mujoco.mj_forward(self.model, scratch)
            error = np.r_[desired - self._tip(scratch), 0.12 * _rotation_error(desired_rot, self._orientation(scratch))]
            if np.linalg.norm(error) < 2e-5:
                break
            mujoco.mj_jacSite(self.model, scratch, jp, jr, self._tip_id)
            jac = np.vstack((jp, 0.12 * jr))
            change = jac.T @ np.linalg.solve(jac @ jac.T + 1e-5 * np.eye(6), error)
            scratch.qpos[:] = np.clip(scratch.qpos + np.clip(change, -0.12, 0.12), self._limits[:, 0] + 0.02, self._limits[:, 1] - 0.02)
        mujoco.mj_forward(self.model, scratch)
        if np.linalg.norm(desired - self._tip(scratch)) > 0.005:
            raise RuntimeError("The initial arm posture could not reach its hover position")
        return scratch.qpos.copy()

    def reset(self, seed: int | None = None) -> None:
        chosen_seed = self._seed if seed is None else self._validate_seed(seed)
        # Validate first; reset is an explicit operation, never an action shortcut.
        self._seed = chosen_seed
        rng = np.random.default_rng(chosen_seed)
        self._phases = rng.uniform(0, 2 * math.pi, (2, 7))
        self._frequencies = rng.uniform(0.12, 1.1, (2, 7))
        mujoco.mj_resetData(self.model, self.data)
        self.data.qpos[:] = self._initial_posture()
        mujoco.mj_forward(self.model, self.data)
        self._home = self.data.qpos.copy()
        self._q_target = self._home.copy()
        self._q_control = self._home.copy()
        self._target_position = self._tip().copy()
        self._control_position = self._target_position.copy()
        self._target_rotation = self._orientation().copy()
        self._mode = "cartesian"
        self._pressure_n = None
        self._force_goal = 0.
        self._force_integral = 0.
        self._filtered_force = 0.
        self._stiffness = 1.
        self._contact_force = np.zeros(3)
        self._contact_torque = np.zeros(7)
        self._contact_active = False
        self._normal_force = 0.
        self._tangential_force = 0.
        self._peak_force = 0.
        self._safety_reason = None
        self._last_drive = self.data.qfrc_bias.copy()
        self.data.ctrl[:] = self._last_drive
        mujoco.mj_forward(self.model, self.data)

    def validate_target(self, position, pressure_n=None, orientation=None, stiffness=None) -> dict:
        """Normalize a complete Cartesian command without changing simulator state."""
        position = _vector(position, 3, "position")
        if np.any(position < [-0.2, 0.15, -0.01]) or np.any(position > [0.25, 0.55, 0.30]):
            raise ValueError("position must lie inside x[-.20,.25], y[.15,.55], z[-.01,.30] metres")
        force = None if pressure_n is None else _number(pressure_n, "pressure_n", 0., MAX_PRESSURE_N)
        compliance = self._stiffness if stiffness is None else _number(stiffness, "stiffness", 0.1, 2.)
        rotation = self._target_rotation.copy()
        if orientation is not None:
            quat = _vector(orientation, 4, "orientation")
            norm = np.linalg.norm(quat)
            if norm < 1e-8:
                raise ValueError("orientation must be a nonzero quaternion [w,x,y,z]")
            rotation = _matrix(quat / norm)
        if force is not None and force > 0 and not self._inside_canvas(position):
            raise ValueError("positive pressure requires x/y inside the canvas")
        return {"position": position.tolist(), "pressure_n": force,
                "orientation": _quaternion(rotation).tolist(), "stiffness": compliance}

    def set_target(self, position, pressure_n=None, orientation=None, stiffness=None) -> None:
        command = self.validate_target(position, pressure_n, orientation, stiffness)
        position = np.array(command["position"])
        force = command["pressure_n"]
        if self._mode != "cartesian":
            self._control_position = self._tip().copy()
        self._mode = "cartesian"
        self._target_position = position.copy()
        self._target_rotation = _matrix(np.array(command["orientation"]))
        self._pressure_n = force
        self._stiffness = command["stiffness"]
        self._safety_reason = None
        if force is None or force == 0:
            self._force_integral = 0.

    def validate_joint_target(self, angles, stiffness=None) -> dict:
        """Normalize a joint command without changing simulator state."""
        angles = _vector(angles, 7, "angles")
        if np.any(angles < self._limits[:, 0]) or np.any(angles > self._limits[:, 1]):
            raise ValueError("angles exceed the model's joint limits; see observe().joint_limits")
        compliance = self._stiffness if stiffness is None else _number(stiffness, "stiffness", 0.1, 2.)
        return {"angles": angles.tolist(), "stiffness": compliance}

    def set_joint_target(self, angles, stiffness=None) -> None:
        command = self.validate_joint_target(angles, stiffness)
        angles = np.array(command["angles"])
        if self._mode != "joints":
            self._q_control = self.data.qpos.copy()
        self._mode = "joints"
        self._q_target = angles.copy()
        self._stiffness = command["stiffness"]
        self._pressure_n = None
        self._force_goal = 0.
        self._force_integral = 0.
        self._safety_reason = None

    def hold(self, unload: bool = False) -> None:
        if not isinstance(unload, bool):
            raise ValueError("unload must be a boolean")
        self._mode = "cartesian"
        self._target_position = self._tip().copy()
        self._target_rotation = self._orientation().copy()
        self._control_position = self._target_position.copy()
        self._pressure_n = None
        self._force_goal = 0.
        self._force_integral = 0.
        if unload:
            self._target_position[2] = max(0.025, self._target_position[2])

    @staticmethod
    def _inside_canvas(position) -> bool:
        return -0.15 <= position[0] <= 0.15 and 0.20 <= position[1] <= 0.50

    def _read_contact(self) -> None:
        force = np.zeros(3)
        torque = np.zeros(7)
        normal_force = 0.
        for index in range(self.data.ncon):
            contact = self.data.contact[index]
            if {contact.geom1, contact.geom2} != {self._nib_id, self._canvas_id}:
                continue
            local_force = np.zeros(6)
            mujoco.mj_contactForce(self.model, self.data, index, local_force)
            # Contact frame rows are world-space axes; force points onto geom2.
            sign = 1. if contact.geom2 == self._nib_id else -1.
            world_force = sign * contact.frame.reshape(3, 3).T @ local_force[:3]
            world_torque = sign * contact.frame.reshape(3, 3).T @ local_force[3:]
            force += world_force
            normal_force += max(0., float(world_force[2]))
            mujoco.mj_applyFT(self.model, self.data, world_force, world_torque,
                             contact.pos, self._pen_body_id, torque)
        self._contact_force = force
        self._contact_torque = torque
        self._normal_force = normal_force
        self._tangential_force = float(np.linalg.norm(force[:2]))
        self._contact_active = normal_force > 0.005
        self._peak_force = max(self._peak_force, normal_force)

    def _cartesian_torque(self) -> np.ndarray:
        position = self._tip()
        velocity = self._jacp @ self.data.qvel
        requested = self._target_position.copy()
        if self._pressure_n is not None:
            requested[2] = 0. if self._pressure_n > 0 else max(requested[2], 0.005)
        distance = requested - self._control_position
        # A command may change abruptly; its internal reference cannot.
        max_step = 0.12 * self.timestep
        change = distance * min(1., max_step / max(np.linalg.norm(distance), 1e-12))
        if requested[2] < 0.005 and self._control_position[2] < 0.04:
            change[2] = max(change[2], -0.012 * self.timestep)
        self._control_position += change
        kp = np.array([550., 550., 350.]) * self._stiffness
        kd = np.array([24., 24., 19.]) * math.sqrt(self._stiffness)
        force = kp * (self._control_position - position) - kd * velocity
        if self._pressure_n is not None and self._pressure_n > 0:
            if self._contact_active or (self._force_goal > 0 and position[2] < 0.003):
                self._force_goal += np.clip(self._pressure_n - self._force_goal, -8 * self.timestep, 8 * self.timestep)
                error = self._force_goal - self._filtered_force
                self._force_integral = float(np.clip(self._force_integral + error * self.timestep * 5., -2., 2.))
                force[2] = -self._force_goal - error - self._force_integral - kd[2] * velocity[2]
            else:
                # Before touch, approach at a bounded speed that decreases with
                # distance. Full target force in free space would cause an impact.
                descent_speed = min(0.06, max(0.0025, 5. * max(position[2], 0.)))
                force[2] = 40. * (-descent_speed - velocity[2])
                self._force_goal = 0.
                self._force_integral *= math.exp(-self.timestep / 0.1)
        else:
            self._force_goal = 0.
        force = np.clip(force, [-18., -18., -8.], [18., 18., 18.])
        rotation_error = _rotation_error(self._target_rotation, self._orientation())
        moment = 2.5 * self._stiffness * rotation_error - 0.32 * (self._jacr @ self.data.qvel)
        moment = np.clip(moment, -2., 2.)
        torque = self._jacp.T @ force + self._jacr.T @ moment
        # Keep the elbow's redundant posture near neutral without imposing it on
        # the task. Project a weak preference through the task's kinematic nullspace.
        jac = np.vstack((self._jacp, 0.15 * self._jacr))
        null = np.eye(7) - jac.T @ np.linalg.solve(jac @ jac.T + 1e-5 * np.eye(6), jac)
        torque += null @ (0.7 * (self._home - self.data.qpos) - 0.1 * self.data.qvel)
        return torque

    def step(self) -> None:
        mujoco.mj_jacSite(self.model, self.data, self._jacp, self._jacr, self._tip_id)
        if self._mode == "cartesian":
            torque = self._cartesian_torque()
        else:
            tau = np.array([0.10, 0.10, 0.08, 0.055, 0.025, 0.02, 0.02])
            change = (self._q_target - self._q_control) * (1 - np.exp(-self.timestep / tau))
            self._q_control += np.clip(change, -1.5 * self.timestep, 1.5 * self.timestep)
            torque = self._joint_kp * self._stiffness * (self._q_control - self.data.qpos)
            torque -= self._joint_kd * math.sqrt(self._stiffness) * self.data.qvel
        # Smooth deterministic motor disturbance; bounded and independent of the
        # external client's observation or command frequency. No white-noise jitter.
        waves = np.sin(2 * math.pi * self._frequencies * self.data.time + self._phases).mean(axis=0)
        torque += waves * np.array([0.012, 0.012, 0.007, 0.006, 0.0015, 0.001, 0.001])
        torque += self.data.qfrc_bias  # gravity and velocity-dependent bias compensation
        self.data.ctrl[:] = np.clip(torque, -self._torque_limits, self._torque_limits)
        self._last_drive = self.data.ctrl.copy()
        mujoco.mj_step(self.model, self.data)
        # Bring Cartesian/contact outputs to the post-integration timestamp.
        mujoco.mj_forward(self.model, self.data)
        self._read_contact()
        self._filtered_force += (self._normal_force - self._filtered_force) * (1 - math.exp(-self.timestep / 0.015))
        if self._normal_force > CONTACT_LIMIT_N:
            self.hold(unload=True)
            self._safety_reason = "contact_force_limit"
        if not np.isfinite(self.data.qpos).all() or not np.isfinite(self.data.qvel).all():
            raise RuntimeError("Simulation produced a nonfinite state; reset is required")

    def pen_sample(self) -> dict:
        position = self._tip()
        shaft = self._orientation()[:, 2]  # points from nib toward the pen tail
        u = (position[0] + 0.15) / 0.30
        v = (0.50 - position[1]) / 0.30
        inside = self._inside_canvas(position)
        proximity = inside and -0.005 <= position[2] <= 0.06 and shaft[2] > 0
        contact = proximity and self._contact_active
        # Tablet +y is down the image, opposite to world +y.
        tilt_x = math.degrees(math.atan2(shaft[0], max(shaft[2], 1e-9)))
        tilt_y = math.degrees(math.atan2(-shaft[1], max(shaft[2], 1e-9)))
        lateral = self._orientation()[:, 0]
        return {
            "u": float(np.clip(u, 0., 1.)), "v": float(np.clip(v, 0., 1.)),
            "pressure": float(np.clip(self._normal_force / MAX_PRESSURE_N, 0., 1.)) if contact else 0.,
            "tilt_x": float(np.clip(tilt_x, -90., 90.)),
            "tilt_y": float(np.clip(tilt_y, -90., 90.)),
            "rotation": float(math.degrees(math.atan2(-lateral[1], lateral[0])) % 360),
            "proximity": bool(proximity), "contact": bool(contact),
        }

    def observe(self) -> dict:
        mujoco.mj_jacSite(self.model, self.data, self._jacp, self._jacr, self._tip_id)
        position = self._tip()
        velocity = self._jacp @ self.data.qvel
        points = {name: self.data.site_xpos[self.model.site(name).id].tolist()
                  for name in ("shoulder", "elbow", "wrist", "grip")}
        points["tip"] = position.tolist()
        actual_orientation = _quaternion(self._orientation()).tolist()
        return {
            "sim_time": float(self.data.time), "joint_names": list(JOINT_NAMES),
            "q": self.data.qpos.tolist(), "dq": self.data.qvel.tolist(),
            "joint_limits": self._limits.tolist(),
            "torque": self.data.qfrc_actuator.tolist(),
            "contact_torque": self._contact_torque.tolist(),
            "drive_saturated": (np.abs(self._last_drive) >= self._torque_limits - 1e-6).tolist(),
            "tip": {"position": position.tolist(), "velocity": velocity.tolist(),
                    "orientation": actual_orientation,
                    "angular_velocity": (self._jacr @ self.data.qvel).tolist()},
            "target": {"mode": self._mode, "position": self._target_position.tolist(),
                       "controller_position": self._control_position.tolist(),
                       "orientation": _quaternion(self._target_rotation).tolist(),
                       "joint_angles": self._q_target.tolist() if self._mode == "joints" else None,
                       "pressure_n": self._pressure_n, "stiffness": self._stiffness},
            "contact": {"active": self._contact_active,
                        "normal_force_n": self._normal_force,
                        "filtered_normal_force_n": self._filtered_force,
                        "tangential_force_n": self._tangential_force,
                        "force_world_n": self._contact_force.tolist(),
                        "peak_normal_force_n": self._peak_force,
                        "sliding": bool(self._contact_active and np.linalg.norm(velocity[:2]) > 0.001)},
            "tracking_error": (self._target_position - position).tolist(),
            "pen": self.pen_sample(), "arm_points": list(points.values()),
            "arm_named_points": points,
            "canvas": {"x": CANVAS_BOUNDS["x"].copy(), "y": CANVAS_BOUNDS["y"].copy(), "z": 0.},
            "safety_reason": self._safety_reason,
        }
