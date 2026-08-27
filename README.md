# Cogwheel

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The central claim — that
> the picture is a machine rather than a curve — is not asserted but measured:
> `cgtest --closure` walks the pen for the full number of turns the tooth counts
> predict and fails if it does not come home, and `cgtest --beer` fails unless
> two pens crossing transmit the *product* of what each transmits alone, which
> is what makes ink subtractive rather than additive. Both probe the same code
> that ships. A control sweep fails if any parameter turns out to do nothing —
> it found one on the first run. Both plugins have since been **loaded into
> Resolume Arena 7.27.1 on macOS and on Windows** — they register with the right
> names, ids and categories, expose all 48 controls with nothing truncated,
> render, and hold a factory preset through live rendering. **No OpenFX host has
> ever loaded the Resolve build**, and nothing here has been used on a show.

A Spirograph, as two plugins for Resolume Arena/Avenue and as an OpenFX plugin
for DaVinci Resolve.

A toothed ring is fixed to the paper. A smaller toothed wheel rolls around
inside it, in mesh, and a pen dropped through one of the wheel's holes is
carried along for the ride. That is the whole machine, and everything the plugin
does falls out of it.

## The one idea

**Nothing here evaluates a hypotrochoid.**

There is a ring with N teeth and a wheel with n teeth, and because gears mesh,
the wheel cannot roll by anything other than a whole tooth at a time. So every
property of the figure follows from those two integers alone — and, crucially,
the operator can see *why*:

| Ring | Wheel | Common factor | Lobes | Turns to close |
| ---: | ----: | ------------: | ----: | -------------: |
|   96 |    32 |            32 |     3 |              1 |
|   96 |    52 |             4 |    24 |             13 |
|   96 |    31 |             1 |    96 |             31 |

96 and 32 mesh three to one, so that figure is a deltoid and it is finished
after a single turn. Move one tooth to 31 and nothing divides any more: the same
machine now takes thirty-one turns and lays down ninety-six lobes. A plugin with
a floating-point "ratio" control makes both of those shapes and explains
neither. The Ring Teeth and Wheel Teeth controls report the lobe count and the
turn count as you drag them.

Three more things fall out of taking the machine seriously:

**The gears slip, and that is what keeps the drawing alive.** A closed figure
retraces its own line for ever — that is not a bug, it is what a Spirograph *is*,
and about ten seconds in nothing moves again. The answer is not a fudge: a real
plastic Spirograph's mesh is not quite true, and a mesh half a percent out
precesses the figure instead of closing it. `Creep` is that mesh. `Skip` is the
jumped tooth that ruins a drawing, which is the other thing real ones do.

**Ink is subtractive.** The sheet accumulates optical density and what you see is
the paper through it — `paper * exp(-density)`. So a second pen crossing the
first gives the product of the two transmissions, which is the muddy near-black
it is on the table and which no amount of additive blending can produce. A pen
that lingers lays down more. And because the buffer never held a colour, you can
change the paper, the palette or the pen mid-drawing without anything already on
the sheet being wrong.

**A drawing is a stack of figures.** When one closes you lift the pen, move to
another hole, change the pen and draw the next on top — which is how the
multicoloured Spirograph drawing everybody remembers is actually made. That is
`Layers`.

If you are ever tempted to draw a spirograph, stop: either it already falls out
of the machine, or the machine is wrong somewhere and that is the bug.

## Two pens, and two of everything else

A real Spirograph came with ballpoint refills, and a ballpoint's ball rolls: it
delivers ink per unit of **distance**, so a line is the same darkness however
fast your hand moved. A fibre tip feeds by capillary action, per unit of
**time**, so it blooms wherever the pen slows down — which at a cusp is a great
deal. `Pen Type` is that choice, and both go through the same closed form; the
only difference is whether a step's quantum of ink is `Flow × dt` or
`Flow × length`. Nothing anywhere computes a speed or divides by one.

`Show Gears` puts the ring, the wheel, the arm and the pen on screen, with the
real tooth counts, so they can be counted.

## The two builds

| | |
| --- | --- |
| **Cogwheel** | A source. Draws on its own sheet of paper. |
| **Cogwheel Ink** | An effect. The clip can be the paper the pen draws on, the ink the pen picks up, or both. |

Honest expectation for **Ink from Clip**: excellent on footage with large areas
of strong colour, and mud on anything busy. That is what a pen does to a
picture, not a setting.

Ink can only ever darken paper, so a pale line on a dark ground is not something
the machine can make. `Print → Negative` is the honest route to it: a drawing,
photographed and printed the other way up.

## Presets

Eight, and each is a recognisable *drawing* rather than a set of slider
positions — which is only possible because the gear train is two integers.
`Classic Rosette` is 96/52 with four pens through four holes. `Three Lobes` is
96/32 with the pen near the rim and the creep turned up, so the deltoid walks
round the sheet as it goes. `Slipping Gear` is the ruined drawing, on purpose.
`Show the Gears` is the machine, cranked slowly, with the overlay on.

`Wet Trail` is the one preset that is dishonest about the machine, and it says
so: a two-second `Fade` turns the accumulating sheet into a moving line. Paper
does not fade. Fade is the only control in the plugin that is not something the
machine can do, and it is there because a VJ needs the sheet to clear.

## First contact

Verified 2026-08-27, on **Resolume Arena 7.27.1 rev 15990**, on two machines:

| | macOS 26.4.1 (Apple GPU) | Windows 11 x64 (Mesa llvmpipe 26.2.0) |
| --- | --- | --- |
| loads | ✅ both bundles | ✅ both DLLs, from the CI artifact |
| registers | ✅ `Cogwheel` CW01 **category 3** (source), `Cogwheel Ink` CW02 **category 1** (effect) | ✅ identical |
| controls | ✅ 48, none over FFGL's 16-character limit | ✅ identical |
| renders | ✅ | ✅ — a different GLSL compiler entirely |
| presets | ✅ applied, **held** through live rendering, dropped to Custom on a real edit | ✅ |
| clean | ✅ no complaint in Arena's log | ✅ no complaint, and no crash dump |

The Windows half matters more than it looks: llvmpipe's GLSL compiler is not
macOS's, so the shaders passing there is real evidence about the shader source
rather than about one vendor's driver. It says nothing about NVIDIA or AMD, and
nothing at all about speed — that box has no GPU.

`Cogwheel Ink` was additionally driven over a stock Gradient source on macOS with
`Ink from Clip` on: the pen picked up the clip's colours and drew the figure out
of them.

## Building and testing

Requires CMake 3.15+ and a C++17 compiler. The FFGL SDK is a submodule.

```
git clone --recursive https://github.com/stoatworks-labs/cogwheel
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build          # into Resolume's Extra Effects folder (macOS)
```

The macOS build is universal by default. The OpenFX bundle is built alongside
and goes to `/Library/OFX/Plugins` by hand; `-DBUILD_OFX=OFF` skips it.

Everything that can be checked without a human:

```
tools/verify.sh
```

That builds universal, checks both bundles export `plugMain` and the OFX bundle
exports `OfxGetPlugin`, ad-hoc signs a copy of the OFX bundle (the failure that
never mentions the plist), and then runs the harness:

| | |
| --- | --- |
| `cgtest --closure` | the lobe and turn counts against an independent gcd, **and** the pen actually coming home after that many turns |
| `cgtest --detail` | total ink is the same at Draft, Normal and Fine |
| `cgtest --rate` | one second of cranking is the same drawing at 24, 60 and 120 fps |
| `cgtest --beer` | two pens crossing multiply, they do not add |
| `cgtest --liveness` | the defaults keep finding fresh sheet; a mesh that is exactly true provably stops |
| `cgtest --presets` | every preset draws something with structure in it |
| `cgtest --defaults` | the constructor's defaults *are* preset 1 |
| `cgtest --hosts` | presets survive all three things a host can do with a value event |
| `cgtest --scale` | the same preset is the same drawing at 320×180 and 1280×720 |
| `cgtest --guard` | a hostile machine leaves no NaN on the sheet |
| `tools/sweep.py` | every parameter changes the drawing |

Render a frame offline:

```
./build/cgtest --out /tmp/frame.png --size 1920x1080 --frames 600 --preset 1
./build/cgtest --contact /tmp/presets.png --frames 900
./build/cgtest --list
```

## Diagnostics

`~/Library/Logs/cogwheel/cogwheel.YYYY-MM-DD.log` — a log file and nothing else:
no crash handler, because a plugin loaded into Resolume has no business
installing a process-wide signal handler. It covers the failures that all look
identical from outside ("it does nothing"): a shader that will not compile, a
sheet that would not allocate, and a preset dropping back to Custom.

## Trademark

*Spirograph* is a registered trademark of Hasbro, Inc. This project is not
affiliated with, endorsed by, or connected to Hasbro in any way. The name is
used here only to describe the kind of machine the plugin models.

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->

## Licence

MIT. See [LICENSE](LICENSE).
