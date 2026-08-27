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

---

## 2026-08-27 — first contact, both platforms, same afternoon

Arena **7.27.1 rev 15990** on macOS 26.4.1 and on the win-lab VM (Mesa llvmpipe
26.2.0, no GPU). Windows was tested with the **CI artifact**, not a local build —
the `.dll` a user would download.

Everything passed, first time, on both:

- **Loads and registers**, with the right categories: `Cogwheel` CW01 category
  **3** (source), `Cogwheel Ink` CW02 category **1** (effect). Identical on both
  platforms. That is the check that caught four bad vectrix releases.
- **All 48 controls** exposed, integer ranges intact (12–360 and 3–240), choice
  parameters carrying their element names. **Nothing truncated** — the only two
  names at FFGL's 16-character limit are the About buttons, and both are
  complete.
- **Renders** on both. The Windows half is the one worth having: llvmpipe's GLSL
  compiler is not macOS's, so the shaders are now known to compile somewhere
  other than one vendor's driver.
- **Presets work live**, which is the fleet's hardest-won behaviour. Applied,
  **held for six seconds of live rendering** while Arena pushed parameters at
  it, and dropped to Custom on a genuine edit — logged by the plugin as
  `preset dropped to Custom: parameter 11 moved to 0.420000`.
- **No complaint in Arena's log** on either platform, and **no crash dump** on
  Windows.
- The plugin's own diagnostics log works on both, at
  `~/Library/Logs/cogwheel/` and `%LOCALAPPDATA%\cogwheel\logs\`.

`Cogwheel Ink` was driven over a stock Gradient source with `Ink from Clip` on,
and the pen picked up the clip's colours and drew the figure out of them.

**One number is worth writing down.** Arena hands an integer back as
`104.99999999999999` for a preset value of 105 — a double round-trip. The
preset-echo comparison's tolerance is a **quantisation allowance of 1e-3**, not a
float epsilon, and this is exactly the case it exists for. At the 1e-4 the fleet
first used, that echo would have read as an operator edit and dropped every
preset to Custom on the next frame.

### Two things this session broke, and one it found

**The CI macOS job was far too slow, and the whole cost is software
rasterisation.** The same suite is **1.5 seconds** on this Mac and **five
minutes** on a runner with no GPU — a factor of about two hundred. The control
sweep was worse: at its local default of 640×360 it was still running after
**twenty-one minutes**, with four Dependabot pull requests queued behind it
doing the same thing.

Fixed in three ways, none of which weakens a check:

- **Smaller raster for the sweep only.** It asks "did this control change ANY
  subpixel", which a coarse raster answers as well as a fine one. CI runs it at
  320×180. Frame counts are deliberately unchanged — several controls only act
  when a figure closes, and cutting frames would report them dead.
- **Parallelism, everywhere it was free.** Every parameter in the sweep is an
  independent pair of `cgtest` processes, and every invariant is an independent
  process with its own GL context; nothing was shared and nothing needed
  ordering. `sweep.py` now uses a thread pool (`--jobs`, default one per core)
  and prints in parameter order regardless of completion order. CI runs the ten
  invariants as background jobs and one `wait`.
- **One number that was simply larger than it needed to be.** `--presets`
  rendered 600 frames per preset, twice each — two fifths of the whole suite.
  It was raised to 600 by trial when `Show the Gears` failed at 180; that
  preset's crank was then retuned and nobody went back. At 400 it measures 2.0%
  covered against a 0.2% floor and 0.073 contrast against a 0.02 floor, and
  every other preset has more margin. The comment now says what the number buys.

⚠️ **`xargs -P … -I{}` is not the tool for this on macOS.** BSD xargs caps the
command line it assembles under `-I` and answers **"command line cannot be
assembled, too long"** for a `sh -c` string of ordinary length. Background jobs
and a single `wait` do the same thing with no limit. (And the first local test
of that block was misleading for a different reason: the Bash tool runs **zsh**,
which does not word-split an unquoted `$checks`, so the loop saw one long word.
GitHub's runners use bash; the block is now pinned with `shell: bash` and was
tested under bash.)

**The check list now lives in two places, so it is compared.** `verify.sh` runs
the checks serially for a person to read; ci.yml runs them at once for a runner.
A check added to one and not the other fails nothing — CI just stops running it,
quietly, for ever. `verify.sh` reads both lists and fails if they differ, and
that guard was tested by removing a check from ci.yml and watching it fire.

⚠️ **The win-lab box is shared, and a deploy hard-kills whoever else is on it.**
The deploy here killed an Arena that had been up since 06:00. Checking first is
the documented rule and it was not followed. What made it look idle afterwards
was right — no log activity for an hour — but that was luck, not method. Related:
three **`apiary` drone processes from a session two days ago** are still running
on this Mac and still holding a connection to the win-lab REST tunnel, which is
what made the box look occupied.

### Not done

- ☠️ **No OpenFX host has ever loaded the Resolve build.** It compiles, the
  bundle is universal, `_OfxGetPlugin` is exported, the plist is right and it
  ad-hoc signs — but nothing has run it. Its CPU renderer is a transcription of
  the GLSL and has never been compared against the GPU build frame for frame;
  that comparison is the obvious next test to write.
- `StoatworksAbout.h` is a hand-written placeholder: cogwheel has no entry in
  the website's `projects.json`, so `sync-about.py` cannot generate it and the
  four About buttons point at pages that do not exist.
- `ATTRIBUTIONS.md` is generated and its component list is right, but
  `stoatworks-backend`'s `attributions/derived.json` has no cogwheel entry. It
  wants an **inspiration** line for the Spirograph itself — the machine this
  sets out to recreate. Deliberately not added here: that master lives in
  another repo and editing it from this session would have left an uncommitted
  change in a shared checkout.
- No release and no browser demo. **CI now runs**: the Windows job goes green in
  1m54s and its artifact is what was tested on win-lab; the macOS job builds and
  passes the invariants and the preset check. `release.yml` has still never
  executed.
