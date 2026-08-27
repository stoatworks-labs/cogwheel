# cogwheel — orientation for another LLM (or a newcomer)

A Spirograph, as two FFGL plugins for Resolume and one OpenFX plugin for
Resolve.

`CLAUDE.md` is the command reference. This file is the *why*.

---

## The one idea

**The figure is what the gears do. Nothing evaluates a curve.**

Cogwheel models a real machine: a toothed ring pinned to paper, a smaller
toothed wheel rolling around inside it in mesh, and a pen dropped through one of
the wheel's holes. `machine/Gears.cpp` turns two tooth counts into a geometry;
`machine/Crank.cpp` turns a hand into an angle; the pen goes where it is
carried, and ink comes off it. There is no hypotrochoid-drawing code and there
must never be one.

That rule is the same one `vectrix` has in a different domain, and it earns its
keep the same way: **if you are tempted to draw the thing, the machine is wrong
somewhere.**

### What falls out of it

- **The ratio is two integers, because gears mesh.** A wheel cannot roll by
  anything but a whole tooth, so 96/32 closes in one turn with three lobes and
  96/31 takes thirty-one turns and has ninety-six. That is not an approximation
  of a float ratio — it is *why* those figures are what they are, and it is why
  `GetParameterDisplay` can tell the operator the answer before they let go of
  the slider.
- **The pen holes are discrete too.** `Snap to Holes` is not a convenience: a
  real wheel has a row of holes and the drawing you get is one of twelve, not
  one of a continuum.
- **Slip is the only honest way to keep a drawing alive.** See the trap below.
- **Ink is subtractive, which makes crossings correct for free.** Beer's law,
  not alpha blending. See `render/Shaders.h`.
- **The multicoloured drawing is a stack of figures**, laid one on top of the
  next as each closes, because that is how a person makes one.

---

## The shape of it

```
per rendered frame:
  1 Clock::Update()      host ms/s calibration, then delta seconds
  2 Resolve()            0..1 params -> engineering units, both hosts
  3 Crank::Advance()     -> a block of Steps and the Runs they fall into
       - solve the geometry from the CURRENT layer's train
       - advance theta, accumulate creep, roll for a skipped tooth
       - on closure: lift the pen, next hole/wheel/colour, maybe wipe
  4 Sheet::Render()
       - fade    multiply the density in place (GL_ZERO, GL_SRC_COLOR)
       - ink     one instanced quad per step interval, additive
       - sheet   paper, grain, exp(-density), negative, gears, composite
```

### Directories

- `source/machine/` — the machine. **No GL, no host, no FFGL.** That is what
  lets the OpenFX build link it unchanged and the harness check the arithmetic
  without a context.
  - `Gears.{h,cpp}` — two integers to a geometry, and a pen position. Pure
    maths.
  - `Wheels.{h,cpp}` — the tooth counts of a real set, and the modelled holes.
  - `Crank.{h,cpp}` — the hand, the slip and the stack of layers.
  - `Pens.{h,cpp}` — pen colours, and the conversion from a colour to an
    absorption. The file that means "ink is subtractive".
  - `Clock.{h,cpp}` — the host clock's unit, measured rather than guessed.
- `source/render/` — the only place the picture exists.
- `source/ofx/` — the CPU mirror of the renderer, and nothing else.

---

## Traps

Ordered by how much time they cost.

### A drawing that has closed is finished, and no amount of tuning fixes it

This is the one that will be reported as a bug, and it is arithmetic.

The figure closes after `n / gcd(N,n)` turns of the ring. From that instant the
pen is on its own line again, and every subsequent turn deposits ink where ink
already is. The picture stops changing. That is what a Spirograph *is* — a
drawing is finished when it is finished — and it is the exact analogue of
escapement's Banach problem in a different mechanism.

There are exactly three ways out, and all three are things a person at the table
actually does:

- **Creep.** The mesh is not quite true. A fraction of a tooth per turn
  precesses the figure so it never quite lands on itself, and the drawing keeps
  growing. It is on by default in the constructor and in preset 1.
- **Skip.** A jumped tooth. Discrete, startling, and the classic way a real
  drawing is ruined.
- **Layers.** Lift the pen when a figure closes and start another.

`cgtest --liveness` is the test that fails if anybody turns all three off in the
defaults. It measures **inked area**, not how much the frame changed, and the
difference is the whole test: a dead machine is not a still one — the pen is
still going round and still darkening the line it is retracing — so "how much did
the picture change" reports a stopped drawing as very much alive. That was the
first version of the test and it was useless. What a dead machine stops doing is
reaching anywhere *new*.

⚠️ **Slip carries across a closure only when the pen never lifts.** A stack of
one figure is somebody who has not stopped turning: the wheel stays in the ring,
`theta` carries on, and the accumulated creep goes with it. A stack of more than
one is a pen being taken out and put back, so the slip is gone. Resetting it in
both cases makes a one-layer drawing retrace its own line for ever; carrying it
in both cases makes every layer of a stack progressively more rotated than the
one before for no reason anybody would recognise.

### The sheet holds density, not a picture

Read `render/Shaders.h` before touching either the ink pass or the sheet pass.

`paper * exp( -density )` is the whole display model and it is not decoration.
It is what makes red over blue come out the near-black it does on paper. It is
what lets the paper colour and the palette change without invalidating what is
already drawn — the buffer never contained a colour to be wrong about. And it is
what makes the plugin's most decisive test possible: at a crossing,

    shown(crossing) × paper = shown(only A) × shown(only B)

with no reference to flow, nib, speed or colour. `cgtest --beer` asserts exactly
that, and additive blending misses it by a mile.

⚠️ **A test that measures a saturated crossing passes trivially.** The first
version of `--beer` ran at a flow where every channel transmitted under 1e-6, so
the identity it checked was `0 = 0 × 0`. It would have passed against a renderer
that added. The flow is now chosen to put every channel in the middle of its
range, and the comment in the test says why.

### Ink per unit distance, or per unit time — and Flow is neither

`Pen Type` is a real distinction between two real classes of pen, not a look.
A ballpoint's ball rolls, so its quantum is `length` and the spreading in the
fragment stage cancels it exactly: the same darkness at any speed. A fibre tip
feeds by capillary action, so its quantum is `dt`, nothing cancels, and the
dwell law falls out.

**`Flow` is multiplied by the nib width in `Controls.cpp`, on purpose.** The
renderer's `flow` is ink per quantum, and a line's peak density goes as
`flow / sigma` — so without that multiply, Nib would be a darkness control with
a width side effect and every preset would need retuning the moment anybody
touched it. It is also the physical answer: a broader nib delivers
proportionally more ink, which is what makes it broader rather than blurrier.

The `scale` factor beside it does the same job for the framing, and cancels
exactly for a ballpoint. It deliberately does **not** cancel for a fibre tip: a
bigger figure at the same crank means a faster nib and a thinner line, which is
correct.

⚠️ **The Flow range was wrong by two orders of magnitude in the first draft**,
because it was copied from a renderer whose deposit was per second and whose
nib was fixed. Every preset rendered as a black scribble. If a whole set of
presets is saturated, suspect the mapping, not the presets.

### A parameter change must not clear the sheet

The opposite of the fleet's usual GPU habit. **The sheet is the drawing.**
Wiping it because somebody nudged the pen colour throws away the thing the
plugin exists to make. Only three things may clear it: `New Sheet`, the crank's
own end-of-stack wipe, and a change of output resolution (which reallocates the
buffer and has no sensible alternative).

### Presets, and the host that ignores your events

Resolume does **not** consume `FF_EVENT_FLAG_VALUE`. It carries on pushing the
values it still believes in, and those arrive as ordinary `SetFloatParameter`
calls carrying a changed value — so the naive "an edit to a covered parameter
drops to Custom" rule fires on the host's own echo, immediately, every time.
Seven plugins in this fleet shipped with that bug.

The fix, carried here from vertigo: keep `hostValues[]` — what the host last
*sent* — separately from what the plugin renders with, and judge by what the
value **is**, never by that it changed. The tolerance is a quantisation
allowance (1e-3), deliberately looser than the 1e-4 used for "did a covered
parameter move?", and a value matching the preset must be **ignored, not
written**.

⚠️ `seedHostValues()` must run **before** `applyPreset` can. Seeding lazily from
`params[]` inside the guard records the preset's own values as the host's
opening position, and the bug comes straight back.

`cgtest --hosts` drives three host behaviours (honours the events / ignores them
/ honours-but-quantises) across every preset, with no GL.

### The OpenFX build replays from the beginning, and cannot do otherwise

Most of this fleet's OFX ports bound their replay at a warm-up window, because a
phosphor forgets and a delay line runs out. **Paper does not.** The sheet at
frame 900 is every stroke since the drawing started, so a warm-up window would
produce a *different drawing* — one missing everything laid down before the
window opened.

So `CogwheelOFX.cpp` replays from frame zero. A linear render costs nothing per
frame beyond that frame's own strokes; scrubbing backwards through a long
drawing is slow and gets slower the further in you are. That is stated in the
plugin description rather than hidden.

### Everything else

- **`Centre` is ±1.5 paper units, not ±2.** At ±2 both ends of Centre Y put the
  whole figure off the sheet, so the control was identical at each end and
  `tools/sweep.py` correctly called it dead. It was the first thing that tool
  found.
- **The paper's tooth is applied at DEPOSIT, not at readout.** That is the
  difference between paper that takes ink unevenly — where a second stroke over
  the same fibre also takes less — and a grey texture over a finished drawing.
  `Grain` (visible tooth) and `Tooth` (uneven take-up) are two controls because
  they are two questions.
- **`CFFGLPlugin::GetParameterDisplay` reads a back-pointer the SDK sets from
  its C entry point**, which is null in an instance the harness constructed
  directly. Falling through to it is a segfault, not a blank column. Hence
  `PlainDisplay`.
- **`cgtest --list` prints an option's ELEMENT range**, not the 0..1 the SDK
  reports, because `tools/sweep.py` parses it and would otherwise sweep every
  dropdown between its first two entries.
- `sample`, `input`, `output`, `filter`, `common`, `active`, `half` are GLSL
  reserved words. Shader errors surface only at runtime, in the diagnostics log.
- FFGL truncates every parameter name at 16 characters. `cgtest --names`.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can
  widen it. `FF_TYPE_INTEGER` is exempt, which is the whole reason the tooth
  counts can be tooth counts.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin at all.
- `cogwheel_core` is an OBJECT library, not STATIC — the plugin registers itself
  from a file-scope constructor nothing references by name.
- macOS builds must be universal. Verify with `lipo`, never the build log.
- The four-character FFGL IDs are `CW01` and `CW02`. `CG01` was already
  cartridge's.
