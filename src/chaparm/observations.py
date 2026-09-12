"""Images of actual simulated ink, arm state, and an optional Windows canvas."""

from io import BytesIO
import math
import sys

from PIL import Image, ImageDraw, ImageGrab


def png_bytes(image):
    stream = BytesIO()
    image.save(stream, format="PNG")
    return stream.getvalue()


class Canvas:
    def __init__(self, size=1024):
        self.size = size
        self.clear()

    def clear(self):
        self.image = Image.new("RGB", (self.size, self.size), "#fffdf8")
        self.previous = None

    def update(self, pen, enabled=True):
        if not enabled or not pen["contact"] or not (0 <= pen["u"] <= 1 and 0 <= pen["v"] <= 1):
            self.previous = None
            return
        point = (pen["u"] * (self.size - 1), pen["v"] * (self.size - 1))
        width = max(1, round(1 + 13 * math.sqrt(max(0, min(1, pen["pressure"])))))
        draw = ImageDraw.Draw(self.image)
        if self.previous is not None:
            draw.line([self.previous, point], fill="#27313b", width=width)
        radius = width / 2
        draw.ellipse((point[0] - radius, point[1] - radius,
                      point[0] + radius, point[1] + radius), fill="#27313b")
        self.previous = point

    def png(self):
        return png_bytes(self.image)


def arm_png(state):
    """A graphics-driver-independent observation, also usable in headless CI."""
    image = Image.new("RGB", (1000, 700), "#101b27")
    draw = ImageDraw.Draw(image)

    def project(point):
        x, y, z = point
        return (int(480 + 850 * (x + 0.55 * y)), int(580 - 850 * (z + 0.32 * y)))

    draw.polygon([project(p) for p in [(-.15, .20, 0), (.15, .20, 0),
                                       (.15, .50, 0), (-.15, .50, 0)]], fill="#eee9df")
    points = state.get("arm_points", [])
    for start, end in zip(points, points[1:]):
        draw.line([project(start), project(end)], fill="#65b5b3", width=17)
    for point in points:
        x, y = project(point)
        draw.ellipse((x-9, y-9, x+9, y+9), fill="#c6e9e5")
    target = state.get("target", {}).get("position")
    if target is not None and state.get("target", {}).get("mode") != "joints":
        x, y = project(target)
        draw.ellipse((x-7, y-7, x+7, y+7), outline="#f6bc68", width=2)
    tip = state["tip"]["position"]
    force = state["contact"]["normal_force_n"]
    if force > .01:
        draw.line([project(tip), project([tip[0], tip[1], tip[2] + min(force*.03, .15)])],
                  fill="#f6bc68", width=4)
    draw.text((28, 24), "CHAPARM / ACTUAL ARM AND CONTACT", fill="#ffffff")
    draw.text((28, 46), f"t={state['sim_time']:.3f}s   normal force={force:.3f} N", fill="#b9cecf")
    return png_bytes(image)


def screen_png(rect):
    if sys.platform != "win32":
        raise RuntimeError("External canvas capture requires Windows")
    if rect is None:
        raise RuntimeError("Configure the visible canvas screen_rect before capture")
    left, top, width, height = rect
    return png_bytes(ImageGrab.grab(bbox=(left, top, left+width, top+height), all_screens=True))
