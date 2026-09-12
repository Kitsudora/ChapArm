"""ctypes publisher for ChapArm's native Wintab provider (not mouse injection)."""

import ctypes as ct
import math
from pathlib import Path
import re
import sys


class Sample(ct.Structure):
    _fields_ = [("size", ct.c_uint32), ("version", ct.c_uint32),
                ("x", ct.c_float), ("y", ct.c_float), ("pressure", ct.c_float),
                ("azimuth_deg", ct.c_float), ("altitude_deg", ct.c_float),
                ("twist_deg", ct.c_float), ("buttons", ct.c_uint32),
                ("proximity", ct.c_uint32), ("timestamp_ms", ct.c_uint64)]


class Status(ct.Structure):
    _fields_ = [(name, ct.c_uint32) for name in
                ("size", "version", "publisher_pid", "consumer_pid", "context_count", "enabled_context_count")]
    _fields_ += [(name, ct.c_uint64) for name in
                 ("last_publish_tick_ms", "last_consumer_tick_ms", "published_samples")]


def validate_rect(rect):
    if (not isinstance(rect, (list, tuple)) or len(rect) != 4
            or any(isinstance(v, bool) or not isinstance(v, int) for v in rect)):
        raise ValueError("screen_rect must contain four integer pixels: left, top, width, height")
    if not (-32768 <= rect[0] <= 32768 and -32768 <= rect[1] <= 32768
            and 1 <= rect[2] <= 16384 and 1 <= rect[3] <= 16384
            and rect[2] * rect[3] <= 33_554_432):
        raise ValueError("screen_rect is out of bounds (maximum 32 megapixels)")
    return tuple(rect)


class Publisher:
    def __init__(self, dll_path, session="default"):
        if sys.platform != "win32":
            raise RuntimeError("The native Wintab publisher requires Windows")
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", session):
            raise ValueError("session must be 1..64 letters, digits, '_' or '-'")
        if ct.sizeof(Sample) != 48 or ct.sizeof(Status) != 48:
            raise RuntimeError("Unexpected native bridge ABI size")
        self.session = session
        self.dll = ct.WinDLL(str(Path(dll_path).resolve(strict=True)), use_last_error=True)
        self.dll.ChapArmOpenPublisher.argtypes = [ct.c_wchar_p]
        self.dll.ChapArmOpenPublisher.restype = ct.c_void_p
        self.dll.ChapArmPublish.argtypes = [ct.c_void_p, ct.POINTER(Sample)]
        self.dll.ChapArmPublish.restype = ct.c_int
        self.dll.ChapArmClosePublisher.argtypes = [ct.c_void_p]
        self.dll.ChapArmClosePublisher.restype = ct.c_int
        self.dll.ChapArmGetStatus.argtypes = [ct.c_wchar_p, ct.POINTER(Status)]
        self.dll.ChapArmGetStatus.restype = ct.c_int
        self.handle = self.dll.ChapArmOpenPublisher(session)
        if not self.handle:
            raise ct.WinError(ct.get_last_error())
        self.user32 = ct.WinDLL("user32", use_last_error=True)
        self.user32.GetSystemMetrics.argtypes = [ct.c_int]
        self.user32.GetSystemMetrics.restype = ct.c_int
        self.user32.GetForegroundWindow.argtypes = []
        self.user32.GetForegroundWindow.restype = ct.c_void_p
        self.user32.GetWindowThreadProcessId.argtypes = [ct.c_void_p, ct.POINTER(ct.c_uint32)]
        self.user32.GetWindowThreadProcessId.restype = ct.c_uint32
        self.kernel32 = ct.WinDLL("kernel32")
        self.kernel32.GetTickCount64.argtypes = []
        self.kernel32.GetTickCount64.restype = ct.c_uint64
        self.enabled = False
        self.last_error = None

    def publish(self, pen, rect, active=True):
        if not self.handle:
            return
        usable = self.enabled and active and rect is not None
        left, top, width, height = rect or (0, 0, 1, 1)
        sx, sy, sw, sh = [self.user32.GetSystemMetrics(i) for i in (76, 77, 78, 79)]
        x = (left + pen.get("u", 0) * (width-1) - sx) / max(1, sw-1)
        y = (top + pen.get("v", 0) * (height-1) - sy) / max(1, sh-1)
        in_canvas = 0 <= pen.get("u", -1) <= 1 and 0 <= pen.get("v", -1) <= 1
        proximity = bool(usable and in_canvas and pen.get("proximity", False))
        touching = proximity and bool(pen.get("contact", False))
        tx = math.tan(math.radians(pen.get("tilt_x", 0)))
        ty = math.tan(math.radians(pen.get("tilt_y", 0)))
        altitude = math.degrees(math.atan2(1, math.hypot(tx, ty)))
        azimuth = math.degrees(math.atan2(tx, -ty)) % 360
        sample = Sample(48, 1, max(0, min(1, x)), max(0, min(1, y)),
                        pen.get("pressure", 0) if touching else 0,
                        azimuth, altitude, pen.get("rotation", 0) % 360,
                        int(touching), int(proximity), 0)
        if not self.dll.ChapArmPublish(self.handle, ct.byref(sample)):
            self.enabled = False
            self.last_error = str(ct.WinError(ct.get_last_error()))
            raise RuntimeError(self.last_error)

    def status(self):
        state = Status()
        state.size, state.version = 48, 1
        ok = self.dll.ChapArmGetStatus(self.session, ct.byref(state))
        result = {name: getattr(state, name) for name, _ in Status._fields_} if ok else {}
        now = self.kernel32.GetTickCount64()
        age = now - state.last_consumer_tick_ms if ok and state.last_consumer_tick_ms else None
        foreground = ct.c_uint32()
        self.user32.GetWindowThreadProcessId(self.user32.GetForegroundWindow(), ct.byref(foreground))
        result.update(consumer_age_ms=age, foreground_pid=foreground.value,
                      ready=bool(ok and state.enabled_context_count and age is not None and age < 500
                                 and state.consumer_pid == foreground.value))
        result.update(available=True, enabled=self.enabled, session=self.session, error=self.last_error)
        return result

    def close(self):
        if self.handle:
            if not self.dll.ChapArmClosePublisher(self.handle):
                raise ct.WinError(ct.get_last_error())
            self.handle = None
