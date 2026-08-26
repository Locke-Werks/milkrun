"""Draw the Milk Run icon: a cream droplet with film-sprocket notches cut into its
trailing edge. Drawn rather than generated, so the geometry is exact and the shape
still reads at 16 pixels."""

import math
import os
from PIL import Image, ImageDraw

BG = (13, 13, 15, 255)
CREAM = (242, 233, 222, 255)
ACCENT = (214, 38, 42, 255)

SS = 8               # supersample factor, downsampled at the end for clean edges
BASE = 256
N = BASE * SS


def droplet_polygon(cx, cy, r, apex_y, steps=720):
    """Outline of a teardrop: an apex joined to a circle by its two tangent lines."""
    ax, ay = cx, apex_y
    d = math.hypot(ax - cx, ay - cy)
    if d <= r:
        raise ValueError("apex must sit outside the bulb")

    # Angle at the centre between the line to the apex and each tangent point.
    theta = math.acos(r / d)
    base = math.atan2(ay - cy, ax - cx)

    left = base + theta
    right = base - theta

    pts = [(ax, ay)]

    # Sweep the bulb the long way round, so the arc forms the bottom of the drop.
    # Sweep increasing so the arc travels through the bottom of the bulb. Going the
    # other way traces the top and yields a crescent instead of a drop.
    a = left
    end = right
    if end < a:
        end += 2 * math.pi
    span = end - a
    for i in range(steps + 1):
        t = a + span * i / steps
        pts.append((cx + r * math.cos(t), cy + r * math.sin(t)))

    return pts


def build(size):
    img = Image.new("RGBA", (N, N), BG)

    # Geometry in normalised units, then scaled. Tuned so the drop fills the frame
    # without touching it.
    cx, cy = N * 0.5, N * 0.60
    r = N * 0.285
    apex_y = N * 0.13

    drop = Image.new("L", (N, N), 0)
    ImageDraw.Draw(drop).polygon(droplet_polygon(cx, cy, r, apex_y), fill=255)

    # Three sprocket notches marching down the trailing (right) edge. Placed on the
    # bulb's circumference so they read as cut into the silhouette.
    notches = Image.new("L", (N, N), 0)
    nd = ImageDraw.Draw(notches)
    side = r * 0.30
    for ang_deg in (-34.0, 0.0, 34.0):
        a = math.radians(ang_deg)
        px = cx + r * math.cos(a)
        py = cy + r * math.sin(a)
        nd.rectangle([px - side * 0.62, py - side / 2, px + side * 0.62, py + side / 2], fill=255)

    # Cream everywhere inside the drop, accent where a notch overlaps it. The part
    # of each notch outside the silhouette simply stays background, which is what
    # gives the bitten-edge look.
    img.paste(CREAM, (0, 0), drop)
    inside_notch = Image.new("L", (N, N), 0)
    inside_notch.paste(notches, (0, 0), drop)
    img.paste(ACCENT, (0, 0), inside_notch)

    return img.resize((size, size), Image.LANCZOS)


def main():
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icon_out")
    os.makedirs(out_dir, exist_ok=True)

    sizes = [16, 24, 32, 48, 64, 128, 256]
    frames = [build(s) for s in sizes]

    ico = os.path.join(out_dir, "milkrun.ico")
    frames[-1].save(ico, format="ICO",
                    sizes=[(s, s) for s in sizes],
                    append_images=frames[:-1])

    # 512 png for the README header and anywhere else a bitmap is wanted.
    build(512).save(os.path.join(out_dir, "milkrun.png"), format="PNG")

    # Contact sheet so the small sizes can be eyeballed.
    sheet = Image.new("RGBA", (sum(s for s in sizes) + 20 * len(sizes), 280), (30, 30, 34, 255))
    x = 10
    for s, f in zip(sizes, frames):
        sheet.paste(f, (x, 140 - s // 2), f)
        x += s + 20
    sheet.save(os.path.join(out_dir, "contact_sheet.png"))

    print("wrote:", ico)
    for s in sizes:
        print("  size", s)


if __name__ == "__main__":
    main()
