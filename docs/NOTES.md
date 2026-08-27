# cogwheel — notes

Working notes for the plugin: what was decided, what was measured, and what went
wrong on the way. `AGENTS.md` is the orientation; this is the record.

---

## 2026-08-27 — the whole thing, in one sitting

Built from the fleet's usual donors: `vectrix` for the CMake shape, the
source/effect split and the OpenFX target; `flipbook` for the parameterised
`cmake/InfoOFX.plist.in`; `orrery` for the preset override machinery. The FFGL
submodule is pinned to `b1afaf9`, the same commit the rest of the fleet is on.

### What it is

A Spirograph modelled as a machine rather than as a curve. The whole design
follows from one decision — **the ratio is two integers because gears mesh** —
and the payoff is that every property of the figure is explained by the two
numbers on screen rather than emerging from a float somebody tuned.

### Measured, not asserted

Numbers from the harness on this machine, at 320×180 unless stated:

| check | result |
| --- | --- |
| `--closure` | eight trains, inside and outside; the pen returns to within **1e-16 to 2e-14** of its start after the predicted number of turns |
| `--detail` | Draft / Normal / Fine (360 / 1440 / 5760 steps a turn) deposit the same total ink to **0.005 %** |
| `--rate` | one second of cranking at 24, 60 and 120 fps deposits the same total ink to **0.001 %** |
| `--beer` | at a crossing, `shown = shown(A) × shown(B)` to **1.4e-8 … 3.0e-8** absolute, on all three channels |
| `--liveness` | the defaults find **2.46 %** fresh sheet in five seconds; a true mesh with one layer finds **0.02 %** |
| `--scale` | eight presets, 320×180 against 1280×720, box-averaged density differs by **0.4 % … 1.1 %** |
| `tools/verify.sh` | 25 checks, fresh universal build included, **22 seconds** |

`--rate` at 0.001 % is worth a second look: the deposit is per-step and each
step carries its own `dt` (or its own length), so frame-rate independence is an
identity rather than a tuning. The residual is the raster.

### Six things that went wrong

**The Flow range was two orders of magnitude too high**, and every preset
rendered as a black scribble. The mapping had been written for a deposit that
was per second against a fixed nib; here the peak density of a line is
`flow / (speed × sigma × √2π)`, which at the default crank and nib came to about
**236**. Anything over about 4 is solid black. Fixed by making Flow mean
*darkness*: it is multiplied by the nib width and by the framing scale, which
cancels the geometry exactly for a ballpoint and leaves the physically correct
residual for a fibre tip.

**`--beer` passed trivially at first.** With the flow that high, every channel
transmitted under 1e-6 and the identity being asserted was `0 = 0 × 0` — a test
that would have passed against an additive renderer. Now the test picks a flow
that puts every channel mid-range, and says so in a comment. **A test whose
inputs saturate is not a test.**

**`--liveness` measured the wrong thing.** The first version compared successive
frames and reported the deliberately-dead machine as alive, which is correct and
useless: a machine that has stopped *drawing* is still going round, still
darkening the line it retraces. Rewritten to measure inked **area** — how much
fresh sheet the pen found in five seconds — after which the live and dead cases
are two orders of magnitude apart.

**`--presets` and `--scale` were both measuring the paper.** Coverage was
counted against a flat reference read from a corner, and Chalkboard's grain
differs from flat at every pixel, so it read as 96 % covered and failed. Both
now difference against the *same preset rendered with the flow shut off*, which
is exact because both renders are deterministic and the grain is a function of
position. `--scale` additionally compares box-averaged **density** rather than
what the display pass shows — `shown` is an exponential of density and averaging
it does not commute — and forces a nib wide enough to be resolved at 320×180,
because a stroke thinner than a pixel is point-sampled and the ink between pixel
centres is simply lost. That is a property of rasters, not of this plugin.

**`cgtest --list` segfaulted.** `CFFGLPlugin::GetParameterDisplay` reads
`m_pPlugin`, a back-pointer the SDK sets from its C entry point — null in an
instance the harness constructed directly. Vectrix has a `PlainDisplay` for
exactly this reason and it was not obvious why until it crashed. Cogwheel now
has one too, and it prints an option's element *name* rather than its index.

**`tools/sweep.py` found `Centre Y` dead on its first run.** Not a uniform
mismatch — a range. Centre was ±2 paper units and the sheet is ±1 tall, so both
ends of Centre Y put the whole figure off the paper and the two renders were
identical. Narrowed to ±1.5. Centre X survived only because the sheet is 16:9
and ±2 still leaves part of the figure visible, which is exactly the kind of
half-broken a sweep is for.

### Decisions worth writing down

**Ink is subtractive, and that is the one idea.** The sheet accumulates optical
density and the display pass is `paper * exp(-density)`. Everything good follows:
red over blue is the product of the two transmissions rather than their sum, a
lingering pen darkens, and the palette and paper can change mid-drawing because
the buffer never held a colour. It also makes the sharpest test in the repo
possible — a parameter-free identity at a crossing.

**The sheet is one buffer, not a ping-pong pair.** The fade is drawn with
`glBlendFunc( GL_ZERO, GL_SRC_COLOR )`, which is a multiply in place. A
ping-pong would cost a second full-resolution `RGBA32F` allocation and a
full-frame copy every frame for a pass whose entire job is one multiply.

**`Print → Negative` exists because ink cannot lighten paper.** A pale line on a
dark ground is not something the machine can make, and pretending otherwise
would have meant a second, dishonest ink model. A photographic negative of a
drawing that *could* be made is a real thing and costs one subtraction.

**`Fade` is the only control that is not something the machine does**, and both
the hint and the README say so. A drawing does not fade. It is there because a
VJ needs the sheet to clear, and giving it a physical-sounding name would have
been a lie.

**Two pen types, because there are two kinds of pen.** A ballpoint's ball rolls
(ink per unit distance, even line at any speed) and a fibre tip feeds by
capillary action (ink per unit time, blooms at the cusps). Both go through the
same closed form; only the quantum differs. The Spirograph came with
ballpoints, so that is the default.

**The classic tooth counts are a convenience, not a citation.** `kSetWheels` and
`kSetRings` in `machine/Wheels.cpp` are what the published part lists give, and
have **not** been checked against a physical set. `Ring` and `Wheel` are free
integers over a wide range precisely so nothing depends on the list being right;
`Snap to Set` merely restricts them. The twelve hole positions are openly a
model — the discreteness is the point, not the specific radii.

### Not done

- ☠️ **No build has been loaded into Resolume or Resolve on any machine.** The
  README's disclaimer says so in those words and should be replaced the moment
  it stops being true. The fleet's experience is that first contact with a real
  host finds what no offline test can.
- `StoatworksAbout.h` is a hand-written placeholder: cogwheel has no entry in
  the website's `projects.json`, so `sync-about.py` cannot generate it and the
  four About buttons point at pages that do not exist.
- `ATTRIBUTIONS.md` is generated and its component list is right, but
  `stoatworks-backend`'s `attributions/derived.json` has no cogwheel entry. It
  wants an **inspiration** line for the Spirograph itself — the machine this
  sets out to recreate. Deliberately not added here: that master lives in
  another repo and editing it from this session would have left an uncommitted
  change in a shared checkout.
- No CI run, no Windows build, no release, no browser demo. The `.github`
  workflows are copied from vectrix and have never executed.
- The OpenFX build has been **compiled and its bundle verified** (universal,
  `_OfxGetPlugin` exported, plist correct, ad-hoc signs) but never loaded by a
  host. Its CPU renderer is a transcription of the GLSL and has not been
  compared against the GPU build frame for frame; that comparison is the obvious
  next test to write.
