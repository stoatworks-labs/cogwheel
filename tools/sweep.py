"""Every parameter must actually change the drawing.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at both ends of its range against the same machine,
and report any that made no difference at all.

    python3 tools/sweep.py

Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**A machine needs time before most of its controls mean anything.** Cogwheel
draws; it does not paint a frame. A control that changes which figure is drawn
shows nothing until enough of the figure has been drawn to tell, and a control
that only acts when a figure CLOSES shows nothing until one has. So the sweep
renders several seconds by default and considerably more for the controls in
`CONTEXT` below, and the crank is wound up so that those seconds are worth
several figures.

**A snap that has nothing to snap looks dead.** Snap to Set at the defaults is
correct and invisible, because 96 and 52 are already tooth counts a real set
carries. It is swept against a wheel of 53, which is not.

**A dropdown holds its element VALUE.** `cgtest --list` prints an option's real
range for exactly this reason -- see the note there.

**The last control has a different NAME in each build.** It is one parameter --
the same id, the same position -- but the source build calls it Opacity and the
effect build calls it Mix, because a source has nothing to mix with. Nothing may
go in `BASE`, which is applied to both, unless it is named the same in both.

**It is embarrassingly parallel and it should be.** Every parameter is an
independent pair of renders in its own `cgtest` process with its own GL context,
so nothing is shared and nothing needs a lock. Serially this is 82 renders; on a
machine with a GPU that is a few seconds either way, but on a CI runner with no
GPU it was **six and a half minutes**. Results are collected and printed in
parameter order regardless of the order they finish, so the output is the same
either way.

**Never sweep the About block.** Those are buttons that open a web browser, and
sweeping them opens one tab per press. `cgtest --list` marks them so.
"""
import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = str(ROOT / "build" / "cgtest")
SCRATCH = tempfile.mkdtemp(prefix="cgsweep")

# Overridable, because the same sweep runs in two places with very different
# budgets. Locally, through `tools/verify.sh`, it is a few seconds at 640x360.
# On a CI runner with no GPU every pixel costs about a hundred times more, and
# 640x360 is four times the pixels of 320x180 for no extra signal -- the
# question here is "did this control change ANY subpixel", which a coarse
# raster answers as well as a fine one. FRAMES is NOT reduced there: several
# controls only act when a figure closes, and cutting the frame count would
# report them dead.
WIDTH, HEIGHT = 640, 360
FRAMES = 240

# Parameters that cannot or must not be swept, with the reason.
SKIP = {
    "New Sheet": "an event whose whole effect is to start the drawing again, "
                 "which at frame zero is what it was already doing",
    "Preset": "a whole machine; every other row here is one of its columns",
}

# The world every sweep starts in. The crank is wound well past the default so
# that a few seconds of rendering is several complete figures -- which is what
# makes the layer controls, the pen set and the wipe visible at all.
BASE = {
    "Crank": 0.90,
    "Layers": 4,
    "Show Gears": 0.0,
}

# 96/32 meshes three to one and closes in a SINGLE turn, so at this crank a
# figure completes about five times a second. Anything that only happens when a
# figure closes is swept on it.
FAST_CLOSE = {"Wheel Teeth": 32, "Crank": 0.90}

CONTEXT = {
    # -- gears -----------------------------------------------------------
    # 53 teeth is not a count any set carries, so snapping has somewhere to
    # move it to. At the default 52 the control is correctly invisible.
    "Snap to Set": {"Wheel Teeth": 53},
    # The pen is swept off a hole for the same reason.
    "Snap to Holes": {"Pen Hole": 0.62},

    # -- the crank -------------------------------------------------------
    # Sync overrides the Crank slider entirely, so at Free it is whatever BASE
    # says and at 8 Bars it is derived from the tempo. Both draw; they draw
    # different amounts.
    "Sync": {},

    # -- slip ------------------------------------------------------------
    "Skip Size": {"Skip Chance": 1.0},

    # -- layers ----------------------------------------------------------
    # All three only do anything at a figure's closure.
    "Layers": dict(FAST_CLOSE),
    "On Closing": dict(FAST_CLOSE, **{"Layers": 6}),
    "Wipe Sheet": dict(FAST_CLOSE, **{"Layers": 2, "_frames": 400}),
    # A stack of one is one pen, so the pen SET cannot show; and the sequence
    # of holes a seed picks only starts at the second layer.
    "Pens": dict(FAST_CLOSE, **{"Layers": 6}),
    "Seed": dict(FAST_CLOSE, **{"Layers": 6, "On Closing": 2}),

    # -- the pen ---------------------------------------------------------
    # The Ink colour is only the pen when the set is Ink Colour; under Four
    # Pens it is correctly ignored.
    "Ink": {"Pens": 0},
    "Ink_Green": {"Pens": 0},
    "Ink_Blue": {"Pens": 0},

    # -- the paper -------------------------------------------------------
    # A fade needs time to have faded something.
    "Fade": {"_frames": 400},
    # Both clip controls are the effect build's alone. On the source build
    # there is no clip, and Controls.cpp forces them off rather than reading a
    # texture that is not bound.
    "Ink from Clip": {"_effect": True},
    "Paper from Clip": {"_effect": True},

    # -- the overlay -----------------------------------------------------
    "Gear Tint": {"Show Gears": 1.0},
    "Gear Tint_Green": {"Show Gears": 1.0},
    "Gear Tint_Blue": {"Show Gears": 1.0},
}


def parameters():
    """id, name, kind, low, high from the harness's own declaration."""
    out = subprocess.run([BIN, "--list"], capture_output=True, text=True)
    if out.returncode != 0:
        print("could not list parameters:", out.stdout, out.stderr)
        sys.exit(1)

    found = []
    for line in out.stdout.splitlines():
        m = re.match(
            r"\s*(\d+)\s+(.+?)\s{2,}(\S+)\s+([\d.eE+-]+)\s+\[\s*([\d.eE+-]+)\s*\.\.\s*([\d.eE+-]+)\s*\]",
            line,
        )
        if m:
            found.append(
                (int(m.group(1)), m.group(2).strip(), m.group(3),
                 float(m.group(5)), float(m.group(6)))
            )
    return found


def render(path, overrides, frames, effect=False):
    args = [BIN, "--out", path, "--size", f"{WIDTH}x{HEIGHT}", "--frames", str(frames)]
    if effect:
        args.append("--effect")
    merged = dict(BASE)
    merged.update({k: v for k, v in overrides.items() if not k.startswith("_")})
    for name, value in merged.items():
        args += ["--set", f"{name}={value}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", " ".join(args), r.stdout, r.stderr)
        sys.exit(1)
    return pathlib.Path(path).read_bytes()


def pixels(png):
    """Raw RGBA out of the harness's own PNG, so nothing else is a dependency."""
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = int.from_bytes(png[i:i + 4], "big")
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(data[0:4], "big")
            height = int.from_bytes(data[4:8], "big")
        elif kind == b"IDAT":
            idat += data
        i += 12 + length

    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray(width * height * 4)
    previous = bytearray(stride)
    pos = 0
    for row in range(height):
        filter_type = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            b = previous[x]
            c = previous[x - 4] if x >= 4 else 0
            if filter_type == 1:
                line[x] = (line[x] + a) & 255
            elif filter_type == 2:
                line[x] = (line[x] + b) & 255
            elif filter_type == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif filter_type == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        out[row * stride:(row + 1) * stride] = line
        previous = line
    return out


def difference(a, b):
    pa, pb = pixels(a), pixels(b)
    if len(pa) != len(pb):
        return 1.0, len(pa)
    changed = sum(1 for x, y in zip(pa, pb) if x != y)
    return changed / max(len(pa), 1), changed


def sweep_one(job):
    """One parameter, both ends. Runs in a worker thread; the work is two
    subprocesses, so the GIL is irrelevant and nothing here is shared."""
    pid, name, low, high, context = job
    frames = context.get("_frames", FRAMES)
    effect = context.get("_effect", False)

    lo = dict(context)
    hi = dict(context)
    lo[name] = context.get("_low", low)
    hi[name] = context.get("_high", high)

    a = render(f"{SCRATCH}/{pid}_lo.png", lo, frames, effect)
    b = render(f"{SCRATCH}/{pid}_hi.png", hi, frames, effect)
    fraction, count = difference(a, b)
    return pid, name, fraction, count


def main():
    global WIDTH, HEIGHT

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--size", default="%dx%d" % (WIDTH, HEIGHT),
                    help="render size, WxH (default %dx%d)" % (WIDTH, HEIGHT))
    ap.add_argument("--jobs", type=int, default=0,
                    help="parallel renders (default: one per core, capped at 8)")
    args = ap.parse_args()
    if "x" in args.size:
        WIDTH, HEIGHT = (int(v) for v in args.size.split("x", 1))

    jobs = args.jobs or min(8, os.cpu_count() or 1)

    if not pathlib.Path(BIN).exists():
        print(f"{BIN} is not built")
        return 1

    skipped = []
    work = []
    for pid, name, kind, low, high in parameters():
        if kind == "about":
            skipped.append((name, "a button that opens a web browser"))
            continue
        if name in SKIP:
            skipped.append((name, SKIP[name]))
            continue
        work.append((pid, name, low, high, CONTEXT.get(name, {})))

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for r in pool.map(sweep_one, work):
            results.append(r)

    # Printed in parameter order, not completion order, so a parallel run and a
    # serial one produce the same report.
    dead = []
    for pid, name, fraction, count in sorted(results):
        # Counts, not percentages: a control that moves three subpixels and one
        # that moves none look identical as a percentage.
        if count == 0:
            dead.append(name)
            print(f"DEAD  {pid:4d}  {name}")
        else:
            print(f"ok    {pid:4d}  {name}  ({count} subpixels, {fraction * 100:.2f}%)")

    print()
    for name, why in skipped:
        print(f"skip  {name}: {why}")

    print(f"\n{len(results)} swept, {len(dead)} dead, {len(skipped)} skipped, {jobs} at a time")
    if dead:
        print("\nDEAD CONTROLS: " + ", ".join(dead))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
