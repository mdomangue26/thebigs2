#!/usr/bin/env python3
"""Compare a native renderer frame with the emulator's (phase 3).

  native_compare.py <emulator.png> <native.png> [side_by_side.png]
  native_compare.py <native_capture_NNN_native dir>

emulator.png: a screenshot of the game (e.g. <outdir>/scene/tNN.png from
tools/native_capture_run.sh); native.png: a resolve the native renderer wrote
(native_capture_NNN_native/resolve_KK_06FFD000.png is the frame D3D presents).
Both 1280x720 RGBA8. With a directory: every emu_resolve_KK_*.png the capture
wrote (--readback_resolve=full, the emulator's result of resolve KK) against
the native resolve_KK_*.png, one line each.

Prints the mean difference, the best pixel alignment, and how much differs once
a per-channel tone curve (native -> emulator) is fitted: the emulator presents
through the game's gamma ramp, the native images are the render target before
it. Pixels still off by more than 24 after the curve are counted and painted in
the difference image (moving backgrounds and animations differ between the two
frames too: the screenshot is taken just before the capture). Writes emulator |
native | difference side by side. Standard library only.
"""
import os, sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from native_contact_sheet import read_png, write_png  # noqa: E402


def main():
    if len(sys.argv) == 2 and os.path.isdir(sys.argv[1]):
        return compare_dir(sys.argv[1])
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    for line in compare(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None):
        print(line)


def compare_dir(d):
    for name in sorted(os.listdir(d)):
        if name.startswith('emu_resolve_') and name.endswith('.png') and os.path.exists(os.path.join(d, name[4:])):
            try:
                lines = compare(os.path.join(d, name), os.path.join(d, name[4:]), None)
            except Exception as e:  # e.g. a PNG cut short when the game was stopped
                print(f"{name[4:]:<28} unreadable: {e}")
                continue
            print(f"{name[4:]:<28} " + " | ".join(lines[i] for i in (0, 1, 3)))


def compare(emu_path, native_path, out_path):
    we, he, emu = read_png(emu_path)
    wn, hn, nat = read_png(native_path)
    w, h = min(we, wn), min(he, hn)

    def lum(rows, x, y):
        r = rows[y]
        return r[x * 4] * 0.3 + r[x * 4 + 1] * 0.59 + r[x * 4 + 2] * 0.11

    # Alignment: sampled luminance difference for shifts of up to 2 pixels.
    shifts = []
    for dy in range(-2, 3):
        for dx in range(-2, 3):
            s = n = 0
            for y in range(8, h - 8, 4):
                for x in range(8, w - 8, 4):
                    s += abs(lum(emu, x, y) - lum(nat, x + dx, y + dy))
                    n += 1
            shifts.append((s / max(n, 1), dx, dy))
    shifts.sort()

    # Per-channel tone curve native -> emulator (mean emulator value per native value).
    sums = [[[0, 0] for _ in range(256)] for _ in range(3)]
    for y in range(h):
        re, rn = emu[y], nat[y]
        for x in range(w):
            for c in range(3):
                s = sums[c][rn[x * 4 + c]]
                s[0] += re[x * 4 + c]
                s[1] += 1
    curve = [[(s[0] / s[1] if s[1] else v) for v, s in enumerate(ch)] for ch in sums]

    raw_diff = residual = 0.0
    off = 0
    out = bytearray(w * 3 * 3 * h)
    for y in range(h):
        re, rn = emu[y], nat[y]
        for x in range(w):
            worst = 0
            for c in range(3):
                e, n = re[x * 4 + c], rn[x * 4 + c]
                raw_diff += abs(e - n)
                d = abs(e - curve[c][n])
                residual += d
                worst = max(worst, d)
            if worst > 24:
                off += 1
            o = (y * w * 3) * 3
            out[o + x * 3:o + x * 3 + 3] = re[x * 4:x * 4 + 3]
            out[o + (w + x) * 3:o + (w + x) * 3 + 3] = rn[x * 4:x * 4 + 3]
            v = min(255, int(worst * 4))
            out[o + (2 * w + x) * 3:o + (2 * w + x) * 3 + 3] = bytes((v, v // 4, 0))
    count = w * h * 3
    lines = [
        f"size {w}x{h}",
        f"mean difference {raw_diff / count:.2f}/255",
        "alignment (mean luminance difference, dx, dy): " +
        ", ".join(f"{s:.2f} {dx:+d},{dy:+d}" for s, dx, dy in shifts[:3]),
        f"after a tone curve: mean difference {residual / count:.2f}/255, "
        f"{100.0 * off / (w * h):.1f}% of pixels off by more than 24",
        "tone curve (native -> emulator, green): " +
        ", ".join(f"{v}->{curve[1][v]:.0f}" for v in (16, 64, 128, 192, 250)),
    ]
    if out_path:
        write_png(out_path, w * 3, h, out)
        lines.append(f"wrote {out_path}")
    return lines


if __name__ == '__main__':
    main()
