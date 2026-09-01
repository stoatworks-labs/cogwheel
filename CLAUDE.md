# cogwheel

A Spirograph — a toothed ring, a wheel rolling in mesh with it, and a pen in one
of the wheel's holes — as **two** FFGL plugins for Resolume Arena/Avenue (a
source, `Cogwheel`, and an effect, `Cogwheel Ink`) plus an OpenFX plugin for
Resolve. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`.
Public MIT repo.

Read `AGENTS.md` before changing the machine, the ink model or the presets.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_OFX=OFF`
- Build: `cmake --build build`
- Install both bundles to Resolume: `cmake --install build`
- Render a frame offline: `./build/cgtest --out /tmp/f.png --frames 600`
- A factory preset: `./build/cgtest --out /tmp/f.png --preset 3`
- The effect over a test clip: `./build/cgtest --out /tmp/f.png --effect`
- Contact sheet of every preset: `./build/cgtest --contact /tmp/sheet.png --frames 900`
- List parameters, with what each currently means: `./build/cgtest --list`
- Set anything by name: `--set "Wheel Teeth=32" --set "Creep=0.5"`
- The project video's frames: `./build/cgtest --sequence DIR --script tools/video.cues
  --size 1920x1080 --seconds 62 --fps 30`

## OpenFX build
Built by default; copy `build/Cogwheel.ofx.bundle` to `/Library/OFX/Plugins`.
Disable with `-DBUILD_OFX=OFF`.

## Verify
- Everything: `tools/verify.sh` (fresh universal build + every check, ~22 s)
- The gear arithmetic, and the pen coming home: `./build/cgtest --closure`
- Total ink is independent of Detail: `./build/cgtest --detail`
- The drawing is independent of frame rate: `./build/cgtest --rate`
- Two pens crossing MULTIPLY: `./build/cgtest --beer`
- The defaults keep drawing, a true mesh stops: `./build/cgtest --liveness`
- Every preset draws something: `./build/cgtest --presets`
- The defaults ARE preset 1: `./build/cgtest --defaults`
- Presets survive every host behaviour: `./build/cgtest --hosts`
- The same drawing at every raster: `./build/cgtest --scale`
- A hostile machine leaves no NaN: `./build/cgtest --guard`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Preset rows the right width and kind: `python3 tools/check_presets.py`

## Notes
- **The figure is what the gears do; nothing evaluates a curve.** Two integers
  in, a geometry out. If you are tempted to draw a spirograph, the machine is
  wrong somewhere.
- **A closed figure retraces its own line for ever, and that is a theorem, not
  a bug.** Creep, Skip or Layers is what keeps a drawing alive; one of them is
  on in the defaults and `--liveness` is the test.
- **Slip carries across a closure ONLY when the pen never lifts** — a stack of
  one is somebody who has not stopped turning.
- **The sheet holds optical density, never a colour.** `paper * exp(-density)`.
  That is what makes crossings correct, and what lets the palette change
  mid-drawing.
- **`--liveness` measures inked AREA, not frame difference.** A dead machine
  still darkens the line it is retracing; what it stops doing is reaching
  anywhere new.
- **`--beer` must run off saturation.** At a high flow the identity it checks is
  `0 = 0 × 0` and it passes against any renderer at all.
- **Flow is multiplied by the nib width and by the scale**, so it means darkness
  rather than ink-per-quantum. Skip it and Nib becomes a darkness control.
- **A parameter change must NOT clear the sheet** — the opposite of the fleet's
  GPU habit. The sheet is the drawing.
- **The paper's tooth is applied at deposit, not at readout.** Grain and Tooth
  are two controls because they are two questions.
- **Presets are an OVERRIDE, not a write** — Resolume does not consume value
  events. `seedHostValues()` must run before `applyPreset` can.
- **The OFX build replays from frame zero**, because paper does not forget.
  Linear renders are cheap; backward scrubs are not.
- **CI runs the invariants CONCURRENTLY**, as background jobs, not through
  `--all`. Each check is its own process with its own GL context. Serially they
  were five minutes on a runner with no GPU and 1.5 seconds on this Mac --
  software rasterisation is the entire cost. `tools/verify.sh` compares its own
  check list against ci.yml's, because a check added to one and not the other
  just stops running.
- ☠️ **Never render or preview a sequence below 24 fps.** `Clock` clamps a frame
  to [1/240, 1/24] s, so a 10 fps preview advances the drawing at 41% speed and
  comes back with half its figures unfinished — with the plugin entirely
  correct.
- **An EVENT cue fires once.** Every other cue is re-applied every frame from
  its time; an event re-applied every frame re-fires its rising edge, and
  `New Sheet` held at 1 wipes the paper sixty times a second.
- **At a section boundary in the cue sheet: parameters first, wipe second**, and
  `Print` after the wipe — it re-renders what is already on the sheet.
- **`Centre` is ±1.5, not ±2** — at ±2 both ends of Centre Y are off the sheet
  and the control reads dead.
- `PlainDisplay` exists because `CFFGLPlugin::GetParameterDisplay` segfaults on
  a harness-constructed instance.
- `sample`, `input`, `output`, `filter`, `common`, `active`, `half` are GLSL
  reserved words. Shader errors surface only at runtime, in the diagnostics log.
- FFGL truncates every parameter name at 16 characters. `cgtest --names`.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can
  widen it; `FF_TYPE_INTEGER` is exempt, which is why tooth counts work.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no
  host can instantiate the plugin at all.
- `cogwheel_core` is an OBJECT library, not STATIC — the plugin registers itself
  from a file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL IDs are `CW01` / `CW02`. `CG01` was already cartridge's.
- Public repo. "Commit" = commit **and** push.

## Not done yet

- **First contact is DONE for FFGL** (Arena 7.27.1, macOS and Windows) **and for
  OpenFX** (Resolve Studio 21.0.2.4 lists both plugins; ofxprobe renders one) —
  2026-08-27, see `docs/NOTES.md`. Still outstanding: the OFX build has never
  been applied to a clip inside Resolve, so its CPU renderer has not been
  compared against the GPU one frame for frame.
- ⚠️ **`~/Library/OFX/Plugins` is silently ignored by Resolve.** The bundle must
  be in `/Library/OFX/Plugins` (needs admin) or reached by `OFX_PLUGIN_PATH`. An
  OFX plugin in the user path looks exactly like a broken one.
- **No browser demo.** `wrangler.toml` does not exist, so there is nothing to
  deploy at `cogwheel-demo.stoatworks-labs.com` and step 1c of the release
  checklist does not apply here. There IS a release: v0.1.0 shipped 2026-08-27
  and v0.2.0 follows it. The line that used to stand here said "no release",
  which was true when it was written and was still being read as true a week
  after the first tag -- it is what made a rename look free when it was not.
- **The classic set's tooth counts in `machine/Wheels.cpp` are unverified.**
  They are what the published part lists give and have not been checked against
  a set in hand; `Ring` and `Wheel` are free integers precisely so that nothing
  depends on them being right. The twelve hole positions are openly a model.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It covers the failures that all look identical
from outside ("it does nothing"): a shader that will not compile, a sheet that
would not allocate, and a preset dropping back to Custom.

    ~/Library/Logs/cogwheel/cogwheel.YYYY-MM-DD.log
