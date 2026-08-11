# SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""
The one picture in the showcase document that is made of dots.

Everything else on the showcase pages is drawn: text set by the typesetter,
shapes placed by "objects insert". A document meant to show what the program
does with photographs needs a photograph, and a photograph that goes into a
public repository must not be anybody's. So this makes one, out of arithmetic,
in about a second, with nothing installed beyond the Python standard library.

The result is a sunrise over the north shore of an island that does not exist:
a graded sky, banded cloud, a sun with a glitter path under it, two headlands
in silhouette and a wet beach that holds the reflection. It is deliberately a
little grainy, because a clean gradient reads as a computer drawing and the
point of the page is to show a photograph.

Writing the PNG by hand rather than through an imaging library is the same
decision made once more: zlib and struct are in every Python, and a showcase
that cannot be rebuilt on a plain machine is not much of a showcase.
"""

import math
import struct
import zlib


def _hash01(x: int, y: int, seed: int) -> float:
    """A repeatable pseudo-random number in [0, 1) for a lattice point."""
    h = (x * 374761393) ^ (y * 668265263) ^ (seed * 1442695040888963407)
    h &= 0xFFFFFFFFFFFFFFFF
    h = (h ^ (h >> 13)) * 1274126177
    h &= 0xFFFFFFFFFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFFFF) / 0x1000000


def _smooth(t: float) -> float:
    return t * t * (3.0 - 2.0 * t)


class Noise:
    """
    Value noise on a lattice, summed over a few octaves.

    Lattices are cached per octave because the picture is walked row by row and
    a row touches the same two lattice rows for its whole length; recomputing
    the hash four times per pixel is what would make this slow enough to notice.
    """

    def __init__(self, seed: int = 7):
        self.seed = seed
        self._rows: dict[tuple[int, int], list[float]] = {}

    def _row(self, gy: int, octave: int, width: int) -> list[float]:
        key = (gy, octave)
        cached = self._rows.get(key)
        if cached is None:
            cached = [_hash01(gx, gy, self.seed + octave * 101) for gx in range(width + 2)]
            self._rows[key] = cached
        return cached

    def at(self, x: float, y: float, octaves: int = 4, scale: float = 64.0) -> float:
        total = 0.0
        amplitude = 1.0
        weight = 0.0
        for octave in range(octaves):
            step = scale / (2**octave)
            gx, gy = x / step, y / step
            x0, y0 = int(gx), int(gy)
            fx, fy = _smooth(gx - x0), _smooth(gy - y0)
            top = self._row(y0, octave, 4096)
            bottom = self._row(y0 + 1, octave, 4096)
            a = top[x0] + (top[x0 + 1] - top[x0]) * fx
            b = bottom[x0] + (bottom[x0 + 1] - bottom[x0]) * fx
            total += (a + (b - a) * fy) * amplitude
            weight += amplitude
            amplitude *= 0.5
        return total / weight


def _mix(a: tuple[float, float, float], b: tuple[float, float, float], t: float) -> tuple[float, float, float]:
    t = 0.0 if t < 0.0 else 1.0 if t > 1.0 else t
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


def _clamp_byte(v: float) -> int:
    return 0 if v < 0 else 255 if v > 255 else int(v)


def _headland(x: float, left: float, right: float, height: float, ridges: tuple[float, ...]) -> float:
    """
    How far one headland stands above the horizon at column x, in pixels.

    Zero outside its own stretch of coast, and zero at both ends of it, so that
    a headland meets the water instead of ending in a cliff the width of one
    pixel. The ridges are a short Fourier sum, which is the cheapest way to get
    a skyline that looks like rock rather than like a hill from a schoolbook.
    """
    if x <= left or x >= right:
        return 0.0
    t = (x - left) / (right - left)
    profile = math.sin(math.pi * t) ** 0.7
    detail = 0.0
    for i, weight in enumerate(ridges):
        detail += weight * math.sin(math.pi * t * (2 * i + 3) + i * 1.7)
    return height * profile * (1.0 + 0.28 * detail)


def sunrise(width: int = 1280, height: int = 800, seed: int = 11) -> bytes:
    """The finished picture, as the bytes of a PNG file."""
    noise = Noise(seed)

    horizon = height * 0.52
    sun_x, sun_y = width * 0.63, horizon - height * 0.055
    sun_r = height * 0.038

    zenith = (26.0, 38.0, 62.0)
    upper = (78.0, 92.0, 124.0)
    band = (206.0, 146.0, 116.0)
    glow = (255.0, 206.0, 138.0)
    deep_sea = (18.0, 30.0, 46.0)
    near_sea = (52.0, 66.0, 84.0)
    sand_wet = (96.0, 92.0, 92.0)
    sand_dry = (146.0, 134.0, 120.0)

    beach = height * 0.80
    rows = []

    for y in range(height):
        row = bytearray()
        append = row.append
        if y < horizon:
            # Sky. Three stops rather than two, so the warm band sits on the
            # horizon instead of washing the whole sky orange.
            t = y / horizon
            if t < 0.62:
                base = _mix(zenith, upper, t / 0.62)
            else:
                base = _mix(upper, band, (t - 0.62) / 0.38)
            for x in range(width):
                r, g, b = base
                # Cloud, stretched sideways so the banding reads as distance.
                cloud = noise.at(x * 0.35, y * 1.6, octaves=4, scale=90.0)
                cloud = (cloud - 0.42) * 2.4
                if cloud > 0.0:
                    lit = 1.0 - min(1.0, abs(y - horizon * 0.72) / (horizon * 0.6))
                    tint = _mix((188.0, 176.0, 186.0), (255.0, 198.0, 150.0), lit)
                    r, g, b = _mix((r, g, b), tint, min(0.72, cloud))
                # The sun, and the halo that makes it look like light rather
                # than a sticker.
                d = math.hypot(x - sun_x, (y - sun_y) * 1.05)
                if d < sun_r * 9.0:
                    halo = max(0.0, 1.0 - d / (sun_r * 9.0)) ** 2.4
                    r, g, b = _mix((r, g, b), glow, halo * 0.85)
                if d < sun_r:
                    r, g, b = _mix((r, g, b), (255.0, 246.0, 222.0), 1.0 - (d / sun_r) ** 6)
                append(_clamp_byte(r))
                append(_clamp_byte(g))
                append(_clamp_byte(b))
        elif y < beach:
            t = (y - horizon) / (beach - horizon)
            base = _mix(deep_sea, near_sea, t**0.7)
            for x in range(width):
                r, g, b = base
                # Swell: long low ripples that widen as they come towards the
                # reader, which is what stops the water reading as a gradient.
                swell = math.sin((x * 0.9 + y * 7.0) * 0.012 + y * 0.05) * 0.5 + 0.5
                ripple = noise.at(x * 0.6, y * 3.0, octaves=3, scale=40.0)
                shade = 0.82 + 0.36 * (swell * 0.45 + ripple * 0.55)
                r, g, b = r * shade, g * shade, b * shade
                # The glitter path: a column under the sun, broken up so it
                # looks like light on water and not a searchlight.
                spread = width * (0.02 + 0.20 * t)
                across = abs(x - sun_x) / spread
                if across < 1.0:
                    sparkle = noise.at(x * 2.0, y * 6.0, octaves=2, scale=14.0)
                    strength = (1.0 - across) ** 2 * (0.25 + 0.95 * max(0.0, sparkle - 0.45) * 2.2)
                    r, g, b = _mix((r, g, b), (255.0, 224.0, 176.0), min(0.9, strength))
                append(_clamp_byte(r))
                append(_clamp_byte(g))
                append(_clamp_byte(b))
        else:
            t = (y - beach) / (height - beach)
            base = _mix(sand_wet, sand_dry, t**1.4)
            for x in range(width):
                r, g, b = base
                grain = noise.at(x * 1.4, y * 1.4, octaves=3, scale=26.0)
                shade = 0.86 + 0.30 * grain
                r, g, b = r * shade, g * shade, b * shade
                # Wet sand holds the sky, strongest just below the water line.
                wet = max(0.0, 1.0 - t * 2.6)
                if wet > 0.0:
                    across = abs(x - sun_x) / (width * 0.30)
                    warm = max(0.0, 1.0 - across) ** 2
                    r, g, b = _mix((r, g, b), _mix((132.0, 138.0, 150.0), (226.0, 186.0, 148.0), warm), wet * 0.6)
                append(_clamp_byte(r))
                append(_clamp_byte(g))
                append(_clamp_byte(b))
        rows.append(row)

    # The headlands, painted after the ground so they can be silhouettes over
    # whatever the sky and sea turned out to be. The near one is darker and
    # bluer than the far one, which is the only cue in the picture that says
    # which of the two is further away.
    lands = (
        (width * 0.68, width * 1.02, height * 0.045, (1.0, 0.4, 0.25), 0.46, (34.0, 40.0, 54.0)),
        (width * -0.06, width * 0.33, height * 0.105, (1.0, 0.55, 0.3, 0.18), 0.20, (16.0, 20.0, 30.0)),
    )
    for left, right, tall, ridges, keep, tint in lands:
        for x in range(width):
            rise = _headland(x, left, right, tall, ridges)
            if rise < 0.4:
                continue
            top = int(horizon - rise)
            for y in range(max(0, top), min(height, int(horizon) + 2)):
                row = rows[y]
                i = x * 3
                row[i] = _clamp_byte(row[i] * keep + tint[0])
                row[i + 1] = _clamp_byte(row[i + 1] * keep + tint[1])
                row[i + 2] = _clamp_byte(row[i + 2] * keep + tint[2])

    # Where the water runs up the sand. Without it the two bands meet along a
    # ruled line, which is the one thing in the picture that would say at a
    # glance that nobody stood there with a camera.
    for x in range(width):
        edge = beach + math.sin(x * 0.011 + 1.2) * height * 0.006 + noise.at(x * 1.0, 0.0, 3, 70.0) * height * 0.012
        for y in range(int(edge - height * 0.030), int(edge + height * 0.022)):
            if not 0 <= y < height:
                continue
            d = abs(y - edge) / (height * 0.026)
            foam = max(0.0, 1.0 - d) ** 1.5 * (0.35 + 0.65 * noise.at(x * 2.2, y * 2.2, 2, 18.0))
            if foam <= 0.02:
                continue
            row = rows[y]
            i = x * 3
            r, g, b = _mix((row[i], row[i + 1], row[i + 2]), (232.0, 232.0, 230.0), min(0.85, foam))
            row[i] = _clamp_byte(r)
            row[i + 1] = _clamp_byte(g)
            row[i + 2] = _clamp_byte(b)

    # Grain over everything, because a photograph has some and a gradient has
    # none, and the eye knows the difference before it knows why.
    for y in range(height):
        row = rows[y]
        for x in range(0, width):
            i = x * 3
            speck = noise.at(x * 3.1, y * 3.1, octaves=1, scale=3.0)
            n = (speck - 0.5) * 11.0
            row[i] = _clamp_byte(row[i] + n)
            row[i + 1] = _clamp_byte(row[i + 1] + n)
            row[i + 2] = _clamp_byte(row[i + 2] + n)

    raw = bytearray()
    for row in rows:
        raw.append(0)  # filter: none
        raw.extend(row)

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + kind
            + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


def write_sunrise(path, width: int = 1280, height: int = 800) -> None:
    with open(path, "wb") as handle:
        handle.write(sunrise(width, height))


if __name__ == "__main__":
    import sys

    write_sunrise(sys.argv[1] if len(sys.argv) > 1 else "sunrise.png")
