/**
 * Cogwheel — browser demo.
 *
 * A Spirograph: a toothed ring pinned to the paper, a wheel rolling in mesh
 * inside it (or round the outside), and a pen dropped through one of the
 * wheel's holes. The one idea, from `AGENTS.md`: **the figure is what the gears
 * do; nothing evaluates a curve.** Two tooth counts go in, a geometry comes
 * out, the hand turns the crank, and ink comes off the pen where it is
 * carried. SW Cogwheel (CW01) draws on its own paper; SW Cogwheel Ink (CW02)
 * can take the clip as the paper, or as the ink the pen picks up.
 *
 * Two halves, and they are not equally faithful here:
 *
 *   **The GPU half is the plugin's own GLSL.** `shaders.js` is the eight
 *   `R"( ... )"` pieces of `source/render/Shaders.cpp`, spliced by
 *   `demo/tools/check_shaders.py --write` and never typed; the same script
 *   compares them character for character, checks they are joined as
 *   vertexSource() and fragmentSource() join them and paired as Sheet::InitGL
 *   pairs them, and `tools/verify.sh` runs it. They run as the plugin's
 *   passes -- fade, ink (one instanced quad per pen step, blended into an
 *   RGBA32F sheet of optical density), compose (Fade by Figure's fold-in) and
 *   the sheet (paper, grain, Beer's law, the gear overlay) -- with the
 *   plugin's blend modes.
 *
 *   **The CPU half is a port.** Between the PORT markers below: Gears.cpp,
 *   Wheels.cpp, Crank.cpp, Pens.cpp, the host-clock branch of Clock.cpp,
 *   Controls.cpp's Resolve() and option names, Presets.h, the constructor's
 *   defaults and GetParameterDisplay from Cogwheel.cpp, and the CPU half of
 *   ProcessOpenGL (SetFloatParameter's preset handling, applyPreset, the wipe).
 *   demo/tools/crosscheck/ runs the machine, the mappings, the presets, the
 *   clock and the display strings through one scenario on the plugin's own C++
 *   and on this block, and requires the two printouts to match to the last bit
 *   (verify.sh runs it). The frame logic and the GL half (Sheet::Render: the
 *   passes, the blending, the per-run draws, the uniforms) are checked by a
 *   reader and by the frame comparison in AGENTS.md, not by that.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **Both plugins, one page.** One class with a flag in the plugin, and both
 * declare the same list. The Plugin dropdown is a new instance -- the drawing
 * starts again on a clean sheet -- but the controls keep their values, as a
 * composition moved between the two does. `?plugin=ink` opens the effect.
 *
 * **Integers are dropdowns.** Ring Teeth, Wheel Teeth, Skip Size and Layers are
 * FF_TYPE_INTEGER with their real ranges, which the kit has no control for, so
 * each is a dropdown of every value. Seed (1 to 9,999) offers 1 to 99. The kit
 * stores the dropdown's index; `toHost` turns it into the plugin's integer.
 *
 * **The colours are three sliders**, not a swatch. Arena 7.27.1 shows a RED,
 * GREEN, BLUE triple as three sliders (issue #17, which is why they are named
 * "Ink Red" and so on), so that is what is here.
 *
 * **Events.** New Sheet is FF_TYPE_EVENT: a toggle the page reads on its
 * rising edge, as the plugin does, then puts back, as a host does. Export XML
 * and Load XML write and read a file in the plugin's Documents folder; a page
 * has neither, so they are not on the panel, and Copy link carries a look
 * instead. The About block is not a parameter here, as on every page in the
 * suite.
 *
 * **Presets.** The plugin's own Preset dropdown, last in the panel, with the
 * plugin's applyPreset (the covered controls, a restart, a clean sheet) and its
 * drop to Custom when a covered control moves. The host-restatement guard
 * (hostIsRestatingItself) is not ported: it exists because Resolume pushes
 * values back at the plugin, and this panel sends only what a visitor moves.
 *
 * **The clock.** The kit's seconds, handed to the port as the host's time with
 * the unit already known (the plugin's seconds-or-milliseconds vote is not
 * ported), and clamped to [1/240, 1/24] s a frame exactly as Clock.cpp clamps
 * it. A redraw while paused is still a frame to the plugin, and the clamp
 * never takes a frame as shorter than 1/240 s.
 *
 * **Tempo.** A browser has no host tempo, so Sync's bar modes run at 120 BPM,
 * the FFGL SDK's own default when a host never sends one.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, GLError, bindTexture } from './vendor/gl.js';
import * as S from './shaders.js';

// ==== BEGIN PORT ===========================================================
// machine/Gears, Wheels, Crank, Pens, Clock; Controls.h/.cpp; Presets.h; and
// the parts of Cogwheel.cpp that are not GL. No DOM and no GL between the two
// markers: demo/tools/crosscheck/run.mjs slices this block out by its markers,
// runs it under node and diffs what it prints against printer.cpp's printout
// from the plugin's own C++.

const f32 = Math.fround;
/** libc++'s std::clamp, std::max and std::min, comparison for comparison. */
const clamp = (v, lo, hi) => (v < lo ? lo : hi < v ? hi : v);
const max2 = (a, b) => (a < b ? b : a);
const min2 = (a, b) => (b < a ? b : a);
/** std::lround: half away from zero (Math.round alone rounds -2.5 to -2). */
const lround = (v) => (v < 0 ? -Math.round(-v) : Math.round(v));

/**
 * The float libm the plugin calls: powf (Exponential) and logf (Absorption).
 * Here they are V8's Math in double, rounded to float, which is within a unit
 * or two in the last place of Apple's. The crosscheck passes `libm` in with
 * the plugin's own results to replay; on the page it does not exist.
 */
// eslint-disable-next-line no-undef
const libmReplay = typeof libm !== 'undefined' ? libm : null;
const powf = libmReplay?.powf ?? ((x, y) => f32(Math.pow(x, y)));
const logf = libmReplay?.logf ?? ((x) => f32(Math.log(x)));

//---------------------------------------------------------------------------
// Step.h
//---------------------------------------------------------------------------
const kMaxSteps = 16384;

/** std::vector< Step >, as one Float32Array of x, y, ink, dt: what the VBO gets. */
class StepBlock {
  constructor() {
    this.data = new Float32Array(kMaxSteps * 4);
    this.size = 0;
  }

  clear() { this.size = 0; }

  push(x, y, ink, dt) {
    const o = this.size * 4;
    this.data[o] = x;
    this.data[o + 1] = y;
    this.data[o + 2] = ink;
    this.data[o + 3] = dt;
    this.size += 1;
  }
}

//---------------------------------------------------------------------------
// Gears.h / Gears.cpp
//---------------------------------------------------------------------------
const kTwoPi = 6.283185307179586;
const kMaxTurns = 512;

const Mesh = { Inside: 0, Outside: 1, Count: 2 };

function Gcd(a, b) {
  a = Math.abs(a);
  b = Math.abs(b);
  while (b !== 0) {
    const t = b;
    b = a % b;
    a = t;
  }
  return a === 0 ? 1 : a;
}

/** Geometry's default member values. */
function NewGeometry() {
  return {
    ringTeeth: 96, wheelTeeth: 52, wheelRadius: 0.0, orbit: 0.0, penRadius: 0.0, spinPerOrbit: 0.0,
    penPhase: 0.0, commonFactor: 1, lobes: 1, turnsToClose: 1, outerRadius: 0.0, innerRadius: 0.0, usable: false,
  };
}

function Solve(train) {
  const g = NewGeometry();
  g.ringTeeth = max2(3, train.ringTeeth);
  g.wheelTeeth = max2(3, train.wheelTeeth);

  const N = g.ringTeeth;
  const n = g.wheelTeeth;

  g.wheelRadius = n / N;

  if (train.mesh === Mesh.Inside) {
    g.orbit = 1.0 - g.wheelRadius;
    g.spinPerOrbit = -g.orbit / g.wheelRadius;
    g.penPhase = 0.0;
  } else {
    g.orbit = 1.0 + g.wheelRadius;
    g.spinPerOrbit = g.orbit / g.wheelRadius;
    g.penPhase = 3.141592653589793;
  }

  g.penRadius = clamp(train.penFraction, 0.0, 1.0) * g.wheelRadius;

  g.commonFactor = Gcd(g.ringTeeth, g.wheelTeeth);
  g.lobes = Math.trunc(g.ringTeeth / g.commonFactor);
  g.turnsToClose = Math.trunc(g.wheelTeeth / g.commonFactor);

  g.outerRadius = Math.abs(g.orbit) + g.penRadius;
  g.innerRadius = Math.abs(Math.abs(g.orbit) - g.penRadius);

  const fits = train.mesh === Mesh.Outside || g.wheelTeeth < g.ringTeeth;
  g.usable = fits && g.turnsToClose <= kMaxTurns && g.orbit > 1.0e-6;
  return g;
}

function PenAt(g, theta, slipTeeth) {
  const spin = g.spinPerOrbit * theta + slipTeeth * kTwoPi / g.wheelTeeth;
  const pen = spin + g.penPhase;
  return {
    x: g.orbit * Math.cos(theta) + g.penRadius * Math.cos(pen),
    y: g.orbit * Math.sin(theta) + g.penRadius * Math.sin(pen),
  };
}

function WheelAngle(g, theta, slipTeeth) {
  return g.spinPerOrbit * theta + slipTeeth * kTwoPi / g.wheelTeeth;
}

//---------------------------------------------------------------------------
// Wheels.h / Wheels.cpp
//---------------------------------------------------------------------------
const kSetWheels = [24, 30, 32, 36, 40, 42, 45, 48, 50, 52, 56, 60, 63, 64, 72, 75, 80, 84];
const kSetRings = [96, 105, 144, 150];
const kHoleCount = 12;
const kInnermostHole = 0.18;
const kOutermostHole = 0.92;

function NearestInTable(value, table) {
  if (!table || table.length <= 0) return value;
  let best = table[0];
  let bestDist = Math.abs(value - best);
  for (let i = 1; i < table.length; i += 1) {
    const dist = Math.abs(value - table[i]);
    if (dist < bestDist) {
      best = table[i];
      bestDist = dist;
    }
  }
  return best;
}

function holeRadius(index) {
  const t = index / (kHoleCount - 1);
  return kInnermostHole + (kOutermostHole - kInnermostHole) * t;
}

function NearestHoleIndex(fraction) {
  const span = kOutermostHole - kInnermostHole;
  const t = (clamp(fraction, 0.0, 1.0) - kInnermostHole) / span;
  const index = lround(t * (kHoleCount - 1));
  return clamp(index, 0, kHoleCount - 1) + 1;
}

function NearestHole(fraction) {
  return holeRadius(NearestHoleIndex(fraction) - 1);
}

//---------------------------------------------------------------------------
// Crank.h / Crank.cpp
//---------------------------------------------------------------------------
const Change = { Hole: 0, Wheel: 1, Both: 2, Nothing: 3, KeepGoing: 4, Count: 5 };
const Sync = { Free: 0, Bar1: 1, Bar2: 2, Bar4: 3, Bar8: 4, Count: 5 };

/** splitmix32, in uint32 arithmetic: Math.imul and >>> 0 are C++'s wraparound. */
function Mix(x) {
  x = (x + 0x9e3779b9) >>> 0;
  x = Math.imul(x ^ (x >>> 16), 0x21f0aaad) >>> 0;
  x = Math.imul(x ^ (x >>> 15), 0x735a2d97) >>> 0;
  return (x ^ (x >>> 15)) >>> 0;
}

function Draw(seed, layer, stream) {
  return Mix((Mix((Math.imul(seed, 2654435761) + (layer >>> 0)) >>> 0) + Math.imul(stream, 0x85ebca6b)) >>> 0);
}

function Unit(v) {
  return v * (1.0 / 4294967296.0);
}

/** CrankParams' default member values. */
function NewCrankParams() {
  return {
    train: { ringTeeth: 96, wheelTeeth: 52, mesh: Mesh.Inside, penFraction: 0.75 },
    snapWheelToSet: false,
    snapPenToHoles: false,
    sync: Sync.Free,
    turnsPerSecond: 0.5,
    bpm: 120.0,
    creepTeethPerTurn: 0.0,
    skipChancePerTurn: 0.0,
    skipTeeth: 1.0,
    layers: 1,
    change: Change.Hole,
    wipeOnRestack: true,
    closeWithinSeconds: 0.0,
    seed: 1,
    stepsPerTurn: 1440,
    palette: null,
    paletteCount: 0,
  };
}

class Crank {
  constructor() {
    this.theta = 0.0;
    this.slipTeeth = 0.0;
    this.figurePhase = 0.0;
    this.figureSeconds = 0.0;
    this.layer = 0;
    this.figuresClosed = 0;
    this.wipe = false;
    this.rng = 1;
    this.seedUsed = 0;
    this.holding = false;
    this.heldLayer = 0;
    this.heldChange = Change.Nothing;
    this.lastChange = Change.Hole;
  }

  Restart(seed) {
    this.theta = 0.0;
    this.slipTeeth = 0.0;
    this.figurePhase = 0.0;
    this.figureSeconds = 0.0;
    this.layer = 0;
    this.wipe = false;
    this.seedUsed = seed;
    this.rng = seed === 0 ? 1 : seed;
    this.holding = false;
    this.heldLayer = 0;
  }

  WipeRequested() { return this.wipe; }
  ClearWipeRequest() { this.wipe = false; }
  Layer() { return this.layer; }
  FiguresClosed() { return this.figuresClosed; }
  FigurePhase() { return this.figurePhase; }
  FigureSeconds() { return this.figureSeconds; }
  Theta() { return this.theta; }
  SlipTeeth() { return this.slipTeeth; }

  CurrentTrain(params) {
    const t = { ...params.train };

    if (params.snapWheelToSet) {
      t.ringTeeth = NearestInTable(t.ringTeeth, kSetRings);
      t.wheelTeeth = NearestInTable(t.wheelTeeth, kSetWheels);
    }
    if (params.snapPenToHoles) t.penFraction = NearestHole(t.penFraction);

    const effLayer = this.holding ? this.heldLayer : this.layer;
    const effChange = this.holding ? this.heldChange : params.change;

    if (effLayer <= 0 || effChange === Change.Nothing || effChange === Change.KeepGoing) return t;

    if (effChange === Change.Wheel || effChange === Change.Both) {
      if (params.snapWheelToSet) {
        const index = Draw(params.seed, effLayer, 0) % kSetWheels.length;
        t.wheelTeeth = kSetWheels[index];
      } else {
        const u = Unit(Draw(params.seed, effLayer, 0));
        const span = max2(4, Math.trunc(params.train.wheelTeeth / 2));
        t.wheelTeeth = max2(3, params.train.wheelTeeth + lround((u * 2.0 - 1.0) * span));
      }
      if (params.train.mesh === Mesh.Inside) t.wheelTeeth = min2(t.wheelTeeth, t.ringTeeth - 1);
    }

    if (effChange === Change.Hole || effChange === Change.Both) {
      if (params.snapPenToHoles) {
        const index = Draw(params.seed, effLayer, 1) % kHoleCount;
        const t01 = index / (kHoleCount - 1);
        t.penFraction = kInnermostHole + (kOutermostHole - kInnermostHole) * t01;
      } else {
        t.penFraction = kInnermostHole + (kOutermostHole - kInnermostHole) * Unit(Draw(params.seed, effLayer, 1));
      }
    }

    return t;
  }

  completeFigure(params) {
    this.figuresClosed += 1;

    const closeAt = kTwoPi * Solve(this.CurrentTrain(params)).turnsToClose;
    const penLifted = max2(1, params.layers) > 1;

    if (!penLifted) {
      this.theta -= closeAt;
      if (this.theta < 0.0) this.theta = 0.0;
      this.figurePhase = 0.0;
      this.figureSeconds = 0.0;
      return;
    }

    this.theta = 0.0;
    this.figurePhase = 0.0;
    this.figureSeconds = 0.0;
    this.slipTeeth = 0.0;

    this.layer += 1;
    if (this.layer >= max2(1, params.layers)) {
      this.layer = 0;
      this.wipe = params.wipeOnRestack;
    }
  }

  /** Advance by `frameSeconds`; `steps` is a StepBlock, `runs` an array. */
  Advance(params, frameSeconds, steps, runs) {
    steps.clear();
    runs.length = 0;
    this.figuresClosed = 0;

    if (this.seedUsed !== params.seed) this.Restart(params.seed);

    if (params.change === Change.KeepGoing) {
      if (!this.holding) {
        this.holding = true;
        this.heldLayer = this.layer;
        this.heldChange = this.lastChange;
      }
    } else {
      this.holding = false;
      this.lastChange = params.change;
    }

    let geometry = Solve(this.CurrentTrain(params));

    if (!(frameSeconds > 0.0) || !geometry.usable) return geometry;

    let turnsPerSecond = params.turnsPerSecond;
    if (params.sync !== Sync.Free) {
      const bars = params.sync === Sync.Bar1 ? 1.0
        : params.sync === Sync.Bar2 ? 2.0
          : params.sync === Sync.Bar4 ? 4.0
            : 8.0;
      const bpm = params.bpm > 1.0 ? params.bpm : 120.0;
      const barSeconds = 240.0 / bpm;
      const figureTime = max2(0.05, bars * barSeconds);
      turnsPerSecond = geometry.turnsToClose / figureTime;
    }

    const closeAt = kTwoPi * geometry.turnsToClose;
    const keepGoing = params.change === Change.KeepGoing;
    const timed = params.closeWithinSeconds > 0.0;

    let spent = 0.0;

    const totalAdvance = Math.abs(turnsPerSecond) * frameSeconds * kTwoPi;
    let remaining = totalAdvance;

    let guard = 0;
    while (remaining > 1.0e-12 && guard++ < 64) {
      const toClose = max2(0.0, closeAt - this.theta);
      const advance = min2(remaining, toClose);

      const turns = advance / kTwoPi;
      let count = Math.ceil(turns * params.stepsPerTurn) + 1;
      count = clamp(count, 2, kMaxSteps - steps.size);
      if (count < 2) break;

      const runSeconds = frameSeconds * (advance / max2(1.0e-12, totalAdvance));
      const dt = runSeconds / (count - 1);

      const creepPerStep = params.creepTeethPerTurn * (advance / kTwoPi) / (count - 1);

      const skipChance = clamp(params.skipChancePerTurn, 0.0, 1.0);

      const run = { first: steps.size, count, colour: [0.0, 0.0, 0.0], layer: this.layer, closes: false };
      if (params.palette !== null && params.paletteCount > 0) {
        const index = this.layer % params.paletteCount;
        run.colour[0] = params.palette[index * 3 + 0];
        run.colour[1] = params.palette[index * 3 + 1];
        run.colour[2] = params.palette[index * 3 + 2];
      }

      for (let i = 0; i < count; i += 1) {
        const t = i / (count - 1);
        const angle = this.theta + advance * t;

        if (i > 0) {
          this.slipTeeth += creepPerStep;

          if (skipChance > 0.0) {
            const thisTurn = Math.floor(angle / kTwoPi);
            const lastTurn = Math.floor((this.theta + advance * ((i - 1) / (count - 1))) / kTwoPi);
            if (thisTurn !== lastTurn) {
              const draw = Draw((params.seed + thisTurn) >>> 0, this.layer, 2);
              if (Unit(draw) < skipChance) {
                const direction = Unit(Draw((params.seed + thisTurn) >>> 0, this.layer, 3)) < 0.5 ? -1.0 : 1.0;
                this.slipTeeth += direction * params.skipTeeth;
              }
            }
          }
        }

        const p = PenAt(geometry, angle, this.slipTeeth);
        steps.push(f32(p.x), f32(p.y), 1.0, i + 1 < count ? f32(dt) : 0.0);
      }

      this.theta += advance;
      remaining -= advance;
      spent += runSeconds;
      this.figureSeconds += runSeconds;
      this.figurePhase = clamp(this.theta / closeAt, 0.0, 1.0);

      const home = this.theta >= closeAt - 1.0e-9;
      const faded = timed && this.figureSeconds > params.closeWithinSeconds;
      const carryOn = keepGoing || faded;
      run.closes = home && !carryOn;
      runs.push(run);

      if (home && carryOn) {
        this.theta -= closeAt;
        if (this.theta < 0.0) this.theta = 0.0;
        this.figurePhase = 0.0;
        this.figureSeconds = 0.0;
      } else if (home) {
        this.completeFigure(params);
        geometry = Solve(this.CurrentTrain(params));
        if (!geometry.usable) break;
      }

      if (steps.size >= kMaxSteps - 2) break;
    }

    if (frameSeconds > spent) this.figureSeconds += frameSeconds - spent;

    return geometry;
  }
}

//---------------------------------------------------------------------------
// Pens.h / Pens.cpp. Float arithmetic, so every operation is rounded to f32.
//---------------------------------------------------------------------------
const PenSet = { Ink: 0, Classic: 1, Six: 2, Spectrum: 3, Graphite: 4, Count: 5 };

const rgb = (r, g, b) => [f32(r), f32(g), f32(b)];
const kClassic = [rgb(0.72, 0.10, 0.12), rgb(0.11, 0.20, 0.62), rgb(0.06, 0.42, 0.24), rgb(0.08, 0.08, 0.10)];
const kSix = [
  rgb(0.72, 0.10, 0.12), rgb(0.85, 0.42, 0.06), rgb(0.06, 0.42, 0.24),
  rgb(0.11, 0.20, 0.62), rgb(0.38, 0.12, 0.55), rgb(0.08, 0.08, 0.10),
];

function hue(h) {
  const s = f32(0.82);
  const v = f32(0.62);
  const x = f32(h * 6.0);
  const i = Math.trunc(Math.floor(x)) % 6;
  const f = f32(x - Math.floor(x));
  const p = f32(v * f32(1.0 - s));
  const q = f32(v * f32(1.0 - f32(s * f)));
  const t = f32(v * f32(1.0 - f32(s * f32(1.0 - f))));
  switch (i < 0 ? i + 6 : i) {
    case 0: return [v, t, p];
    case 1: return [q, v, p];
    case 2: return [p, v, t];
    case 3: return [p, q, v];
    case 4: return [t, p, v];
    default: return [v, p, q];
  }
}

function PenCount(set, layers) {
  switch (set) {
    case PenSet.Ink: return 1;
    case PenSet.Classic: return kClassic.length;
    case PenSet.Six: return kSix.length;
    case PenSet.Graphite: return 1;
    case PenSet.Spectrum: return max2(1, layers);
    default: return 1;
  }
}

function PenColour(set, index, layers, ink) {
  const count = PenCount(set, layers);
  const i = count > 0 ? ((index % count) + count) % count : 0;
  switch (set) {
    case PenSet.Ink: return [ink[0], ink[1], ink[2]];
    case PenSet.Classic: return [...kClassic[i]];
    case PenSet.Six: return [...kSix[i]];
    case PenSet.Graphite: return rgb(0.34, 0.34, 0.36);
    case PenSet.Spectrum:
    default: return hue(f32(i / max2(1, count)));
  }
}

function BuildPalette(set, layers, ink, out, capacity) {
  const wanted = min2(max2(1, layers), capacity);
  for (let i = 0; i < wanted; i += 1) {
    const c = PenColour(set, i, layers, ink);
    out[i * 3] = c[0];
    out[i * 3 + 1] = c[1];
    out[i * 3 + 2] = c[2];
  }
  return wanted;
}

const kAbsorptionFloor = f32(1.0e-4);
function Absorption(colour) {
  return [0, 1, 2].map((i) => f32(-logf(clamp(colour[i], kAbsorptionFloor, 1.0))));
}

//---------------------------------------------------------------------------
// Clock.cpp, the branch a host with a clock takes once its unit is known.
// This host's unit is seconds, so the vote is not ported.
//---------------------------------------------------------------------------
const kMinFrameSeconds = 1.0 / 240.0;
const kMaxFrameSeconds = 1.0 / 24.0;

class Clock {
  constructor() {
    this.now = 0.0;
    this.lastNow = -1.0;
    this.frameSeconds = 1.0 / 60.0;
  }

  Update(hostTime) {
    const scaled = hostTime * 1.0;
    if (this.lastNow >= 0.0) {
      const delta = scaled - this.lastNow;
      this.frameSeconds = clamp(delta, kMinFrameSeconds, kMaxFrameSeconds);
      this.now += this.frameSeconds;
    } else {
      this.frameSeconds = 1.0 / 60.0;
      this.now = 0.0;
    }
    this.lastNow = scaled;
  }

  Now() { return this.now; }
  FrameSeconds() { return this.frameSeconds; }
}

//---------------------------------------------------------------------------
// Controls.h: ParamIds, option names, and Resolve().
//---------------------------------------------------------------------------
const PT = {
  RESET: 0,
  RING: 1, WHEEL: 2, MESH: 3, PEN: 4, SNAP_SET: 5, SNAP_HOLES: 6,
  SYNC: 7, RATE: 8, DETAIL: 9, SEED: 10,
  CREEP: 11, SKIP: 12, SKIP_TEETH: 13,
  LAYERS: 14, CHANGE: 15, UNLESS_FADED: 16, WIPE: 17,
  PEN_SET: 18, INK_R: 19, INK_G: 20, INK_B: 21, PEN_TYPE: 22, BLEND: 23, FLOW: 24, NIB: 25, SPREAD: 26, INK_FROM_CLIP: 27,
  PAPER_R: 28, PAPER_G: 29, PAPER_B: 30, PAPER_FROM_CLIP: 31, PAPER_CLEAR: 32, GRAIN: 33, TOOTH: 34, FADE: 35,
  FADE_FIGURES: 36, PRINT: 37,
  ZOOM: 38, CENTRE_X: 39, CENTRE_Y: 40,
  GEARS: 41, GEAR_R: 42, GEAR_G: 43, GEAR_B: 44,
  MIX: 45,
  PRESET: 46, EXPORT: 47, LOAD: 48,
  ABOUT_TEXT: 49,
};
/** params[] up to the About block: what an operator can set. */
const kSlots = PT.ABOUT_TEXT;

const MESH_NAMES = ['Inside the Ring', 'Outside the Ring'];
const SYNC_NAMES = ['Free', '1 Bar', '2 Bars', '4 Bars', '8 Bars'];
const DETAIL_NAMES = ['Draft', 'Normal', 'Fine'];
const CHANGE_NAMES = ['Next Hole', 'Next Wheel', 'Wheel and Hole', 'Same Figure', 'Keep Going'];
const PENSET_NAMES = ['Ink Colour', 'Four Pens', 'Six Pens', 'Spectrum', 'Graphite'];
const PRINT_NAMES = ['Positive', 'Negative'];
const PENTYPE_NAMES = ['Ballpoint', 'Fibre Tip'];
const BLEND_NAMES = ['Multiply', 'Cover', 'Lift'];

const Detail = { Draft: 0, Normal: 1, Fine: 2, Count: 3 };
const Blend = { Multiply: 0, Cover: 1, Lift: 2, Count: 3 };
const kMaxLayers = 16;

const Option = (value, count) => clamp(lround(value), 0, count - 1);
function Exponential(value, low, high) {
  const t = clamp(value, 0.0, 1.0);
  return f32(low * powf(f32(high / low), t));
}
function Linear(value, low, high) {
  return f32(low + f32(f32(high - low) * clamp(value, 0.0, 1.0)));
}

function StepsPerTurn(detail) {
  switch (detail) {
    case Detail.Draft: return 360;
    case Detail.Fine: return 5760;
    default: return 1440;
  }
}

/** Sheet::RenderParams' default member values (floats). */
function NewRenderParams() {
  return {
    flow: 1.0, perDistance: true, nibSigma: f32(0.004), nibSpread: f32(0.15), densityFloor: f32(1.0e-5),
    paperColour: [f32(0.94), f32(0.92), f32(0.86)], paperFromClip: false, paperGrain: f32(0.15), tooth: f32(0.20),
    toothScale: 220.0, fadeSeconds: 0.0, fadeByFigure: false, negative: false, clearPaper: false,
    scale: f32(0.95), centre: [0.0, 0.0], inkFromClip: false, blend: Blend.Multiply,
    gearLevel: 0.0, gearColour: [f32(0.55), f32(0.52), f32(0.48)], opacity: 1.0, passthrough: 0.0,
    frameSeconds: f32(1.0 / 60.0), clearSheet: false,
  };
}

const boolean = (v) => v > 0.5;
const integer = (v) => lround(v);

function Resolve(p, bpm, overInput) {
  const r = {
    crank: NewCrankParams(),
    render: NewRenderParams(),
    detail: Detail.Normal,
    penSet: PenSet.Classic,
    palette: new Float32Array(kMaxLayers * 3),
    paletteCount: 0,
  };

  r.crank.train.ringTeeth = clamp(integer(p[PT.RING]), 12, 360);
  r.crank.train.wheelTeeth = clamp(integer(p[PT.WHEEL]), 3, 240);
  r.crank.train.mesh = Option(p[PT.MESH], Mesh.Count);
  r.crank.train.penFraction = Linear(p[PT.PEN], f32(kInnermostHole), f32(kOutermostHole));
  r.crank.snapWheelToSet = boolean(p[PT.SNAP_SET]);
  r.crank.snapPenToHoles = boolean(p[PT.SNAP_HOLES]);

  r.crank.sync = Option(p[PT.SYNC], Sync.Count);
  r.crank.turnsPerSecond = Exponential(p[PT.RATE], f32(0.01), 10.0);
  r.crank.bpm = bpm;
  r.crank.seed = max2(1, integer(p[PT.SEED])) >>> 0;

  r.detail = Option(p[PT.DETAIL], Detail.Count);
  r.crank.stepsPerTurn = StepsPerTurn(r.detail);

  r.crank.creepTeethPerTurn = Linear(p[PT.CREEP], 0.0, 1.0);
  r.crank.skipChancePerTurn = Linear(p[PT.SKIP], 0.0, 1.0);
  r.crank.skipTeeth = clamp(integer(p[PT.SKIP_TEETH]), 1, 12);

  r.crank.layers = clamp(integer(p[PT.LAYERS]), 1, kMaxLayers);
  r.crank.change = Option(p[PT.CHANGE], Change.Count);
  r.crank.wipeOnRestack = boolean(p[PT.WIPE]);

  r.render.scale = Exponential(p[PT.ZOOM], f32(0.1), 4.0);
  r.render.centre[0] = Linear(p[PT.CENTRE_X], -1.5, 1.5);
  r.render.centre[1] = Linear(p[PT.CENTRE_Y], -1.5, 1.5);

  r.penSet = Option(p[PT.PEN_SET], PenSet.Count);

  const ink = [clamp(p[PT.INK_R], 0.0, 1.0), clamp(p[PT.INK_G], 0.0, 1.0), clamp(p[PT.INK_B], 0.0, 1.0)];

  r.paletteCount = BuildPalette(r.penSet, r.crank.layers, ink, r.palette, kMaxLayers);
  r.crank.palette = r.palette;
  r.crank.paletteCount = r.paletteCount;

  r.render.nibSigma = Exponential(p[PT.NIB], f32(0.0004), f32(0.04));
  r.render.nibSpread = Linear(p[PT.SPREAD], 0.0, 1.5);

  r.render.perDistance = Option(p[PT.PEN_TYPE], 2) === 0;
  r.render.blend = Option(p[PT.BLEND], Blend.Count);

  const darkness = Exponential(p[PT.FLOW], f32(0.05), 50.0);
  r.render.flow = f32(darkness * r.render.nibSigma);
  if (r.render.perDistance) {
    r.render.flow = f32(r.render.flow * r.render.scale);
  } else {
    const kFibreReferenceSpeed = 4.0;
    r.render.flow = f32(r.render.flow * kFibreReferenceSpeed);
  }

  r.render.inkFromClip = overInput && boolean(p[PT.INK_FROM_CLIP]);
  r.render.paperFromClip = overInput && boolean(p[PT.PAPER_FROM_CLIP]);

  r.render.paperColour[0] = clamp(p[PT.PAPER_R], 0.0, 1.0);
  r.render.paperColour[1] = clamp(p[PT.PAPER_G], 0.0, 1.0);
  r.render.paperColour[2] = clamp(p[PT.PAPER_B], 0.0, 1.0);
  r.render.clearPaper = boolean(p[PT.PAPER_CLEAR]);
  r.render.paperGrain = Linear(p[PT.GRAIN], 0.0, 1.0);
  r.render.tooth = Linear(p[PT.TOOTH], 0.0, f32(0.9));
  r.render.toothScale = 220.0;

  r.render.fadeSeconds = p[PT.FADE] <= f32(0.001) ? 0.0 : Exponential(p[PT.FADE], 120.0, 1.0);
  r.render.fadeByFigure = boolean(p[PT.FADE_FIGURES]);
  r.crank.closeWithinSeconds = boolean(p[PT.UNLESS_FADED]) ? r.render.fadeSeconds : 0.0;

  r.render.negative = Option(p[PT.PRINT], 2) === 1;

  r.render.gearLevel = clamp(p[PT.GEARS], 0.0, 1.0);
  r.render.gearColour[0] = clamp(p[PT.GEAR_R], 0.0, 1.0);
  r.render.gearColour[1] = clamp(p[PT.GEAR_G], 0.0, 1.0);
  r.render.gearColour[2] = clamp(p[PT.GEAR_B], 0.0, 1.0);

  const mix = clamp(p[PT.MIX], 0.0, 1.0);
  if (overInput) {
    r.render.passthrough = f32(1.0 - mix);
    r.render.opacity = 1.0;
  } else {
    r.render.passthrough = 0.0;
    r.render.opacity = mix;
  }

  return r;
}

//---------------------------------------------------------------------------
// Presets.h. Standard parameters as the host's 0..1, integers and options
// as their real values -- as floats, which is what params[] holds.
//---------------------------------------------------------------------------
const kPresetParamIDs = [
  PT.RING, PT.WHEEL, PT.MESH, PT.PEN, PT.SNAP_SET, PT.SNAP_HOLES,
  PT.RATE, PT.DETAIL, PT.CREEP, PT.SKIP, PT.SKIP_TEETH,
  PT.LAYERS, PT.CHANGE, PT.WIPE,
  PT.PEN_SET, PT.PEN_TYPE, PT.BLEND, PT.INK_R, PT.INK_G, PT.INK_B, PT.FLOW, PT.NIB, PT.SPREAD,
  PT.PAPER_R, PT.PAPER_G, PT.PAPER_B, PT.GRAIN, PT.TOOTH, PT.FADE, PT.FADE_FIGURES, PT.PRINT,
  PT.ZOOM, PT.GEARS,
];

const CARTRIDGE = [0.94, 0.92, 0.86];
const preset = (name, v) => ({ name, v: Float32Array.from(v) });
/* eslint-disable max-len */
const kPresets = [
  //                       ring wheel mesh pen    snapS snapH rate   det creep skip skipT lay chg wipe pen type bl inkR  inkG  inkB  flow   nib    spr   paper           grain tooth  fade   figf prt zoom   gears
  preset('Classic Rosette', [96, 52, 0, 0.770, 1, 1, 0.737, 1, 0.06, 0.00, 1, 4, 0, 1, 1, 0, 0, 0.72, 0.10, 0.12, 0.625, 0.398, 0.10, ...CARTRIDGE, 0.15, 0.222, 0.000, 0, 0, 0.610, 0.00]),
  preset('Three Lobes', [96, 32, 0, 0.973, 1, 1, 0.466, 2, 0.50, 0.00, 1, 1, 3, 0, 4, 0, 0, 0.34, 0.34, 0.36, 0.699, 0.287, 0.05, ...CARTRIDGE, 0.20, 0.278, 0.000, 0, 0, 0.610, 0.00]),
  preset('Dense Web', [105, 64, 0, 0.500, 1, 1, 0.826, 2, 0.02, 0.00, 1, 6, 2, 1, 3, 0, 0, 0.72, 0.10, 0.12, 0.534, 0.199, 0.05, ...CARTRIDGE, 0.10, 0.111, 0.000, 0, 1, 0.610, 0.00]),
  preset('Flower', [96, 45, 1, 0.838, 1, 1, 0.667, 1, 0.05, 0.00, 1, 6, 0, 1, 2, 0, 0, 0.72, 0.10, 0.12, 0.625, 0.398, 0.20, ...CARTRIDGE, 0.15, 0.222, 0.000, 0, 0, 0.486, 0.00]),
  preset('Slipping Gear', [96, 52, 0, 0.770, 1, 0, 0.667, 1, 0.45, 0.30, 3, 1, 3, 0, 0, 0, 0, 0.55, 0.09, 0.10, 0.593, 0.398, 0.30, ...CARTRIDGE, 0.25, 0.389, 0.374, 0, 0, 0.610, 0.00]),
  preset('Wet Trail', [96, 33, 0, 0.700, 1, 0, 0.900, 2, 0.03, 0.00, 1, 1, 3, 0, 3, 1, 0, 0.72, 0.10, 0.12, 0.778, 0.199, 0.00, ...CARTRIDGE, 0.08, 0.111, 0.855, 0, 0, 0.610, 0.00]),
  preset('Chalkboard', [105, 40, 0, 0.635, 1, 1, 0.566, 1, 0.08, 0.00, 1, 3, 0, 1, 4, 1, 0, 0.34, 0.34, 0.36, 0.492, 0.548, 0.60, ...CARTRIDGE, 0.55, 0.667, 0.000, 0, 1, 0.564, 0.00]),
  preset('Show the Gears', [144, 60, 0, 0.770, 1, 1, 0.520, 1, 0.04, 0.00, 1, 4, 0, 1, 1, 0, 0, 0.72, 0.10, 0.12, 0.625, 0.398, 0.10, ...CARTRIDGE, 0.15, 0.222, 0.000, 0, 0, 0.596, 0.75]),
];
/* eslint-enable max-len */
const kPresetCount = kPresets.length;

//---------------------------------------------------------------------------
// Cogwheel.cpp: the declarations (names, types, ranges, elements), the
// constructor's defaults, and GetParameterDisplay.
//---------------------------------------------------------------------------
const PARAM_INFO = [
  { name: 'New Sheet', type: 'event' },
  { name: 'Ring Teeth', type: 'integer', min: 12, max: 360 },
  { name: 'Wheel Teeth', type: 'integer', min: 3, max: 240 },
  { name: 'Mesh', type: 'option', elements: MESH_NAMES },
  { name: 'Pen Hole', type: 'standard' },
  { name: 'Snap to Set', type: 'boolean' },
  { name: 'Snap to Holes', type: 'boolean' },
  { name: 'Sync', type: 'option', elements: SYNC_NAMES },
  { name: 'Speed', type: 'standard' },
  { name: 'Detail', type: 'option', elements: DETAIL_NAMES },
  { name: 'Seed', type: 'integer', min: 1, max: 9999 },
  { name: 'Creep', type: 'standard' },
  { name: 'Skip Chance', type: 'standard' },
  { name: 'Skip Size', type: 'integer', min: 1, max: 12 },
  { name: 'Layers', type: 'integer', min: 1, max: kMaxLayers },
  { name: 'On Closing', type: 'option', elements: CHANGE_NAMES },
  { name: 'Unless Faded', type: 'boolean' },
  { name: 'Wipe Sheet', type: 'boolean' },
  { name: 'Pens', type: 'option', elements: PENSET_NAMES },
  { name: 'Ink Red', type: 'colour' },
  { name: 'Ink Green', type: 'colour' },
  { name: 'Ink Blue', type: 'colour' },
  { name: 'Pen Type', type: 'option', elements: PENTYPE_NAMES },
  { name: 'Blend', type: 'option', elements: BLEND_NAMES },
  { name: 'Flow', type: 'standard' },
  { name: 'Nib', type: 'standard' },
  { name: 'Pressure', type: 'standard' },
  { name: 'Ink from Clip', type: 'boolean' },
  { name: 'Paper Red', type: 'colour' },
  { name: 'Paper Green', type: 'colour' },
  { name: 'Paper Blue', type: 'colour' },
  { name: 'Paper from Clip', type: 'boolean' },
  { name: 'Clear Paper', type: 'boolean' },
  { name: 'Grain', type: 'standard' },
  { name: 'Tooth', type: 'standard' },
  { name: 'Fade', type: 'standard' },
  { name: 'Fade by Figure', type: 'boolean' },
  { name: 'Print', type: 'option', elements: PRINT_NAMES },
  { name: 'Size', type: 'standard' },
  { name: 'Centre X', type: 'standard' },
  { name: 'Centre Y', type: 'standard' },
  { name: 'Show Gears', type: 'standard' },
  { name: 'Gear Tint Red', type: 'colour' },
  { name: 'Gear Tint Green', type: 'colour' },
  { name: 'Gear Tint Blue', type: 'colour' },
  { name: 'Opacity', type: 'standard' }, // "Mix" on the effect: NameOf()
  { name: 'Preset', type: 'option', elements: ['Custom', ...kPresets.map((p) => p.name)] },
  { name: 'Export XML', type: 'event' },
  { name: 'Load XML', type: 'file' },
];

const NameOf = (id, overInput) => (id === PT.MIX ? (overInput ? 'Mix' : 'Opacity') : PARAM_INFO[id]?.name ?? '?');

/** The groups SetParamGroup gives each run of ids; New Sheet has none. */
function GroupOf(id) {
  if (id === PT.RESET) return '';
  if (id <= PT.SNAP_HOLES) return 'Gears';
  if (id <= PT.SEED) return 'Crank';
  if (id <= PT.SKIP_TEETH) return 'Slip';
  if (id <= PT.WIPE) return 'Layers';
  if (id <= PT.INK_FROM_CLIP) return 'Pen';
  if (id <= PT.PRINT) return 'Paper';
  if (id <= PT.CENTRE_Y) return 'Framing';
  if (id <= PT.GEAR_B) return 'Overlay';
  if (id <= PT.MIX) return 'Output';
  if (id <= PT.LOAD) return 'Preset';
  return 'About';
}

/** CogwheelPlugin's constructor: preset 1's row, then what no preset covers. */
function ConstructorDefaults() {
  const params = new Float32Array(kSlots);
  for (let j = 0; j < kPresetParamIDs.length; j += 1) params[kPresetParamIDs[j]] = kPresets[0].v[j];
  params[PT.SYNC] = Sync.Free;
  params[PT.SEED] = 1.0;
  params[PT.RESET] = 0.0;
  params[PT.CENTRE_X] = 0.5;
  params[PT.CENTRE_Y] = 0.5;
  params[PT.INK_FROM_CLIP] = 0.0;
  params[PT.PAPER_FROM_CLIP] = 0.0;
  params[PT.UNLESS_FADED] = 0.0;
  params[PT.GEAR_R] = 0.36;
  params[PT.GEAR_G] = 0.40;
  params[PT.GEAR_B] = 0.46;
  params[PT.MIX] = 1.0;
  params[PT.PRESET] = 1.0;
  return params;
}

/**
 * printf's %.Nf. Number.prototype.toFixed rounds an exact tie up, printf
 * rounds it to even ("%.0f" of 0.5 is "0", (0.5).toFixed(0) is "1"); a tie is
 * a double whose exact decimal expansion stops at the digit after the last
 * one kept, and is a 5.
 */
function cfix(x, digits) {
  const negative = x < 0 || Object.is(x, -0);
  const a = Math.abs(x);
  let text = a.toFixed(digits);
  const exact = a.toFixed(100);
  const point = exact.indexOf('.');
  const tail = exact.slice(point + 1 + digits);
  if (/^50*$/.test(tail)) {
    const kept = exact.slice(0, point + 1 + digits).replace(/\.$/, '');
    const last = Number(kept[kept.length - 1]);
    if (last % 2 === 0) text = kept;
  }
  return negative ? `-${text}` : text;
}

function PlainDisplay(id, p) {
  const info = PARAM_INFO[id];
  if (info.type === 'option') {
    const element = max2(0, lround(p[id]));
    return element < info.elements.length ? info.elements[element] : '?';
  }
  if (info.type === 'boolean' || info.type === 'event') return p[id] > 0.5 ? 'on' : 'off';
  if (info.type === 'integer') return String(lround(p[id]));
  return cfix(p[id], 4);
}

/** GetParameterDisplay, for every control but the About block. */
function Display(id, p, crank, overInput, bpm = 120.0) {
  const resolved = Resolve(p, bpm, overInput);
  const train = crank.CurrentTrain(resolved.crank);
  const g = Solve(train);
  const c = resolved.crank;
  const r = resolved.render;

  switch (id) {
    case PT.RING:
      return g.usable ? `${train.ringTeeth}t - ${g.lobes} lobes` : `${train.ringTeeth}t - too small`;
    case PT.WHEEL:
      return g.usable ? `${train.wheelTeeth}t closes ${g.turnsToClose}` : `${train.wheelTeeth}t - too big`;
    case PT.PEN:
      return c.snapPenToHoles
        ? `hole ${NearestHoleIndex(train.penFraction)} of ${kHoleCount}`
        : `${cfix(100.0 * train.penFraction, 0)}% of wheel`;
    case PT.RATE:
      return c.sync !== Sync.Free ? 'set by Sync' : `${cfix(c.turnsPerSecond, 2)} turns/s`;
    case PT.CREEP:
      return `${cfix(c.creepTeethPerTurn, 2)} teeth/turn`;
    case PT.SKIP:
      return c.skipChancePerTurn <= 0.0 ? 'never' : `${cfix(100.0 * c.skipChancePerTurn, 0)}% a turn`;
    case PT.NIB:
      return `${cfix(f32(f32(100.0 * r.nibSigma) * 0.5), 2)}% of sheet`;
    case PT.FADE:
      return r.fadeSeconds <= 0.0 ? 'never - paper' : `${cfix(r.fadeSeconds, 0)} s`;
    case PT.FLOW:
      return `${cfix(r.flow, 3)} ${r.perDistance ? 'a unit' : 'a second'}`;
    case PT.ZOOM:
      return `${cfix(r.scale, 2)}x`;
    case PT.UNLESS_FADED:
      if (c.closeWithinSeconds <= 0.0 && p[PT.UNLESS_FADED] > 0.5) return 'no fade: closes';
      if (c.closeWithinSeconds > 0.0) return `longer than ${cfix(c.closeWithinSeconds, c.closeWithinSeconds < 9.95 ? 1 : 0)}s`;
      return PlainDisplay(id, p);
    case PT.LAYERS:
      if (c.change === Change.KeepGoing) return `${c.layers} - keeps going`;
      if (c.layers <= 1) return '1 - never lifts';
      return `${c.layers} figures`;
    case PT.EXPORT:
      return 'ready';
    case PT.LOAD:
      return 'none';
    default:
      return PlainDisplay(id, p);
  }
}

//---------------------------------------------------------------------------
// The CPU half of CogwheelPlugin: SetFloatParameter (less the About buttons,
// Export XML, Load XML and the host-restatement guard), applyPreset, and
// ProcessOpenGL up to the point where it hands Sheet::Render its arguments.
//---------------------------------------------------------------------------
const seedOf = (p) => max2(1, lround(p[PT.SEED])) >>> 0;

class CogwheelInstance {
  constructor(overInput) {
    this.overInput = overInput;
    this.params = ConstructorDefaults();
    this.clock = new Clock();
    this.crank = new Crank();
    this.crank.Restart(1);
    this.steps = new StepBlock();
    this.runs = [];
    this.geometry = NewGeometry();
    this.clearRequested = true;
    /** The SDK's own default: a host that never sends beat info leaves it at 120. */
    this.bpm = 120.0;
    /** The ids RaiseParamEvent was called for, for the page to re-read. */
    this.raised = new Set();
  }

  SetFloatParameter(index, value) {
    if (index < 0 || index >= kSlots) return;
    value = f32(value);

    if (index === PT.RESET) {
      if (value > 0.5 && this.params[PT.RESET] <= 0.5) {
        this.crank.Restart(seedOf(this.params));
        this.clearRequested = true;
      }
      this.params[PT.RESET] = value;
      return;
    }

    if (index === PT.EXPORT || index === PT.LOAD) return;

    if (index === PT.PRESET) {
      const chosen = lround(value);
      if (chosen !== lround(this.params[PT.PRESET])) this.applyPreset(chosen);
      return;
    }

    const previous = this.params[index];
    this.params[index] = value;

    const active = lround(this.params[PT.PRESET]);
    if (active > 0 && Math.abs(f32(value - previous)) > f32(1e-4) && kPresetParamIDs.includes(index)) {
      this.params[PT.PRESET] = 0.0;
      this.raised.add(PT.PRESET);
    }
  }

  applyPreset(presetIndex) {
    this.params[PT.PRESET] = presetIndex;
    if (presetIndex <= 0 || presetIndex > kPresetCount) return;

    const row = kPresets[presetIndex - 1];
    for (let j = 0; j < kPresetParamIDs.length; j += 1) {
      const id = kPresetParamIDs[j];
      if (Math.abs(f32(this.params[id] - row.v[j])) <= f32(1e-6)) continue;
      this.params[id] = row.v[j];
      this.raised.add(id);
    }

    this.crank.Restart(seedOf(this.params));
    this.clearRequested = true;
  }

  /** ProcessOpenGL's CPU half. Returns what Sheet::Render is handed. */
  ProcessFrame(hostTime) {
    this.clock.Update(hostTime);

    const resolved = Resolve(this.params, this.bpm, this.overInput);
    this.geometry = this.crank.Advance(resolved.crank, this.clock.FrameSeconds(), this.steps, this.runs);

    if (this.crank.WipeRequested()) {
      this.clearRequested = true;
      this.crank.ClearWipeRequest();
    }

    resolved.render.frameSeconds = f32(this.clock.FrameSeconds());
    resolved.render.clearSheet = this.clearRequested;
    this.clearRequested = false;

    return resolved;
  }

  /** The line ProcessOpenGL logs when a train cannot be threaded (#16). */
  UnusableNote(resolved) {
    if (this.geometry.usable) return null;
    const train = this.crank.CurrentTrain(resolved.crank);
    return `nothing to draw: a ${train.wheelTeeth}-tooth wheel cannot run inside a ${train.ringTeeth}-tooth ring - set Mesh to Outside, or use a smaller wheel`;
  }
}

// ==== END PORT =============================================================

//===========================================================================
// The GL half: Sheet::Render, with the plugin's passes, buffers and blends.
//===========================================================================

/** setOverBlend, setMultiplyBlend, setUnderBlend from GLState.h. */
function overBlend(gl) { gl.enable(gl.BLEND); gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA); }
function multiplyBlend(gl) { gl.enable(gl.BLEND); gl.blendFunc(gl.ZERO, gl.SRC_COLOR); }
function underBlend(gl) { gl.enable(gl.BLEND); gl.blendFunc(gl.ONE_MINUS_DST_ALPHA, gl.ONE); }

class SheetRenderer {
  constructor(gl, quad, programs, floatLinear) {
    this.gl = gl;
    this.quad = quad;
    this.programs = programs;
    // PaperBuffer::Smooth is GL_LINEAR, which on an RGBA32F texture needs
    // OES_texture_float_linear in WebGL2. The sheet pass reads the paper at its
    // own size, texel for texel, so NEAREST reads the same values without it.
    const filter = floatLinear ? 'linear' : 'nearest';
    this.paper = new PassBuffer(gl, { filter });
    this.settled = new PassBuffer(gl, { filter });
    this.settledInUse = false;
    this.sheetWidth = 0;
    this.sheetHeight = 0;

    // One buffer of Steps, read twice: attribute 0 from the start, attribute 1
    // one Step in, both per instance.
    this.inkVAO = gl.createVertexArray();
    this.inkVBO = gl.createBuffer();
    gl.bindVertexArray(this.inkVAO);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.inkVBO);
    gl.bufferData(gl.ARRAY_BUFFER, kMaxSteps * 16, gl.STREAM_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 4, gl.FLOAT, false, 16, 0);
    gl.vertexAttribDivisor(0, 1);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 16, 16);
    gl.vertexAttribDivisor(1, 1);
    gl.bindVertexArray(null);
    gl.bindBuffer(gl.ARRAY_BUFFER, null);

    // One white texel, wherever the clip would be on the source build.
    this.blankTexture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.blankTexture);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, 1, 1, 0, gl.RGBA, gl.FLOAT, new Float32Array([1, 1, 1, 1]));
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.bindTexture(gl.TEXTURE_2D, null);
  }

  /** A new instance: DeInitGL's buffers gone, so the next frame allocates. */
  reset() {
    this.paper.dispose();
    this.settled.dispose();
    this.settledInUse = false;
    this.sheetWidth = 0;
    this.sheetHeight = 0;
  }

  /** PaperBuffer::Ensure: reallocate only on a change, and clear what is new. */
  ensure(buffer, width, height) {
    const before = buffer.texture;
    buffer.ensure(width, height, this.gl.RGBA32F);
    if (buffer.texture !== before) buffer.clearTo(0, 0, 0, 0);
  }

  compose(from, into, over) {
    const gl = this.gl;
    into.bind();
    if (over) overBlend(gl);
    else underBlend(gl);
    const shader = this.programs.compose.use();
    bindTexture(gl, 0, from.texture);
    shader.setSampler('Source', 0);
    this.quad.draw();
    bindTexture(gl, 0, null);
  }

  /**
   * Sheet::Render. `steps` a StepBlock, `runs` the crank's runs, `rp` the
   * resolved RenderParams; `target` the framebuffer standing in for the host's
   * (null is the canvas); `clipTexture` null on the source build.
   */
  render(steps, runs, geometry, theta, slipTeeth, rp, target, viewport, clipTexture, maxU, maxV) {
    const gl = this.gl;
    const outputWidth = max2(1, viewport[2]);
    const outputHeight = max2(1, viewport[3]);
    const sheetAspect = f32(outputWidth / outputHeight);
    const clipOrBlank = clipTexture ?? this.blankTexture;

    const resized = this.sheetWidth !== outputWidth || this.sheetHeight !== outputHeight;
    this.ensure(this.paper, outputWidth, outputHeight);
    this.sheetWidth = outputWidth;
    this.sheetHeight = outputHeight;

    if (rp.clearSheet || resized) this.paper.clearTo(0, 0, 0, 0);

    const fading = rp.fadeSeconds > 0.0 && rp.frameSeconds > 0.0;
    const byFigure = fading && rp.fadeByFigure;
    const retain = fading ? f32(Math.exp(f32(-rp.frameSeconds / rp.fadeSeconds))) : 1.0;

    if (byFigure) {
      this.ensure(this.settled, outputWidth, outputHeight);
      if (rp.clearSheet || resized || !this.settledInUse) this.settled.clearTo(0, 0, 0, 0);
      this.settledInUse = true;

      this.settled.bind();
      multiplyBlend(gl);
      this.programs.fade.use().set('Retain', retain);
      this.quad.draw();
    } else if (this.settledInUse) {
      this.compose(this.settled, this.paper, false);
      this.settled.dispose();
      this.settledInUse = false;
    }

    if (steps.size > 0) {
      gl.bindBuffer(gl.ARRAY_BUFFER, this.inkVBO);
      gl.bufferSubData(gl.ARRAY_BUFFER, 0, steps.data, 0, steps.size * 4);
      gl.bindBuffer(gl.ARRAY_BUFFER, null);
    }

    if (fading && !byFigure) {
      this.paper.bind();
      multiplyBlend(gl);
      this.programs.fade.use().set('Retain', retain);
      this.quad.draw();
    }

    const inkRuns = (from, to) => {
      if (from >= to || rp.flow <= 0.0) return;

      this.paper.bind();
      overBlend(gl);

      const ink = this.programs.ink.use();
      ink.set('Flow', rp.flow);
      ink.set('PerDistance', rp.perDistance ? 1.0 : 0.0);
      ink.set('NibSigma', max2(rp.nibSigma, f32(1.0e-5)));
      ink.set('NibSpread', max2(rp.nibSpread, 0.0));
      ink.set('DensityFloor', max2(rp.densityFloor, 0.0));
      ink.set('Scale', max2(rp.scale, f32(1.0e-4)));
      ink.set('Centre', rp.centre[0], rp.centre[1]);
      ink.set('SheetAspect', sheetAspect);
      ink.set('Tooth', max2(rp.tooth, 0.0));
      ink.set('ToothScale', max2(rp.toothScale, 1.0));
      ink.set('MaxUV', maxU, maxV);
      ink.set('Blend', rp.blend);

      const clipInk = rp.inkFromClip && clipTexture !== null;
      ink.set('InkFromClip', clipInk ? 1.0 : 0.0);
      ink.setSampler('ClipTexture', 0);
      bindTexture(gl, 0, clipOrBlank);

      gl.bindVertexArray(this.inkVAO);
      gl.bindBuffer(gl.ARRAY_BUFFER, this.inkVBO);

      for (let r = from; r < to; r += 1) {
        const run = runs[r];
        const segments = run.count - 1;
        if (segments <= 0) continue;
        if (run.first + run.count > steps.size) throw new GLError('a run runs past the end of the step block');

        const absorb = Absorption(run.colour);
        ink.set('InkAbsorb', absorb[0], absorb[1], absorb[2]);

        const base = run.first * 16;
        gl.vertexAttribPointer(0, 4, gl.FLOAT, false, 16, base);
        gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 16, base + 16);
        gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, segments);
      }

      gl.bindBuffer(gl.ARRAY_BUFFER, null);
      gl.bindVertexArray(null);
      bindTexture(gl, 0, null);
    };

    if (!byFigure) {
      inkRuns(0, runs.length);
    } else {
      let from = 0;
      while (from < runs.length) {
        let to = from;
        while (to < runs.length && !runs[to].closes) to += 1;
        const closed = to < runs.length;
        if (closed) to += 1;

        inkRuns(from, to);

        if (closed) {
          this.compose(this.paper, this.settled, true);
          this.paper.clearTo(0, 0, 0, 0);
        }
        from = to;
      }
    }

    // The sheet, into the host's framebuffer.
    gl.bindFramebuffer(gl.FRAMEBUFFER, target);
    gl.viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    overBlend(gl);

    const sheet = this.programs.sheet.use();
    bindTexture(gl, 0, this.paper.texture);
    bindTexture(gl, 1, clipOrBlank);
    bindTexture(gl, 2, this.settledInUse ? this.settled.texture : this.blankTexture);

    sheet.setSampler('PaperTexture', 0);
    sheet.setSampler('ClipTexture', 1);
    sheet.setSampler('SettledTexture', 2);
    sheet.set('UseSettled', this.settledInUse ? 1.0 : 0.0);
    sheet.set('MaxUV', maxU, maxV);
    sheet.set('SheetAspect', sheetAspect);

    sheet.set('PaperColour', rp.paperColour[0], rp.paperColour[1], rp.paperColour[2]);
    sheet.set('PaperFromClip', rp.paperFromClip && clipTexture !== null ? 1.0 : 0.0);
    sheet.set('PaperGrain', max2(rp.paperGrain, 0.0));
    sheet.set('ToothScale', max2(rp.toothScale, 1.0));
    sheet.set('Negative', rp.negative ? 1.0 : 0.0);
    sheet.set('ClearPaper', rp.clearPaper ? 1.0 : 0.0);
    sheet.set('HasClip', clipTexture !== null ? 1.0 : 0.0);

    sheet.set('Opacity', clamp(rp.opacity, 0.0, 1.0));
    sheet.set('Passthrough', clipTexture !== null ? clamp(rp.passthrough, 0.0, 1.0) : 0.0);

    sheet.set('GearLevel', clamp(rp.gearLevel, 0.0, 1.0));
    sheet.set('GearColour', rp.gearColour[0], rp.gearColour[1], rp.gearColour[2]);
    sheet.set('Scale', max2(rp.scale, f32(1.0e-4)));
    sheet.set('Centre', rp.centre[0], rp.centre[1]);
    sheet.set('RingRadius', 1.0);
    sheet.set('WheelRadius', f32(geometry.wheelRadius));
    sheet.set('RingTeeth', geometry.ringTeeth);
    sheet.set('WheelTeeth', geometry.wheelTeeth);
    sheet.set('WheelCentre', f32(geometry.orbit * Math.cos(theta)), f32(geometry.orbit * Math.sin(theta)));
    sheet.set('WheelAngle', f32(WheelAngle(geometry, theta, slipTeeth)));
    const pen = PenAt(geometry, theta, slipTeeth);
    sheet.set('PenPoint', f32(pen.x), f32(pen.y));

    this.quad.draw();

    bindTexture(gl, 2, null);
    bindTexture(gl, 1, null);
    bindTexture(gl, 0, null);
    gl.activeTexture(gl.TEXTURE0);
    gl.disable(gl.BLEND);
  }

  dispose() {
    const gl = this.gl;
    this.reset();
    gl.deleteVertexArray(this.inkVAO);
    gl.deleteBuffer(this.inkVBO);
    gl.deleteTexture(this.blankTexture);
  }
}

//===========================================================================
// The renderer the kit drives.
//===========================================================================

const hooks = { beforeFrame: null, afterFrame: null, newInstance: null };

function createRenderer(gl, quad) {
  // Sheet.h: the sheet is RGBA32F and every pass into it blends. Rendering into
  // a float target needs EXT_color_buffer_float (the kit asks for it); blending
  // into a 32-bit one needs EXT_float_blend. Half-float would blend without it,
  // but it runs out of mantissa exactly where a dense figure is darkest -- the
  // reason the plugin does not use it -- so the page stops rather than draw a
  // plausible wrong drawing.
  if (!gl.getExtension('EXT_float_blend')) {
    throw new GLError('This browser has no EXT_float_blend. Cogwheel’s sheet accumulates ink by blending into a 32-bit float buffer, as the plugin does; half-float would stop darkening where a dense figure is darkest, so the page stops here rather than draw a plausible wrong drawing.');
  }
  const floatLinear = !!gl.getExtension('OES_texture_float_linear');

  // Sheet::InitGL's four programs, assembled as Shaders.cpp assembles them.
  const vertexSource = (body) => S.CONSTANTS + body;
  const fragmentSource = (body) => S.CONSTANTS + S.FRAGMENT_HELPERS + body;
  const inkAttribs = { attribs: { stepA: 0, stepB: 1 } };
  const programs = {
    ink: new Program(gl, vertexSource(S.INK_VERTEX_BODY), fragmentSource(S.INK_FRAGMENT_BODY), 'ink', inkAttribs),
    fade: new Program(gl, vertexSource(S.SCREEN_VERTEX_BODY), fragmentSource(S.FADE_FRAGMENT_BODY), 'fade'),
    compose: new Program(gl, vertexSource(S.SCREEN_VERTEX_BODY), fragmentSource(S.COMPOSE_FRAGMENT_BODY), 'compose'),
    sheet: new Program(gl, vertexSource(S.SCREEN_VERTEX_BODY), fragmentSource(S.SHEET_FRAGMENT_BODY), 'sheet'),
  };

  const sheet = new SheetRenderer(gl, quad, programs, floatLinear);

  let instance = null;
  let instanceVariant = null;
  let stats = null;

  return {
    floatLinear,
    get programs() { return programs; },
    get instance() { return instance; },
    get stats() { return stats; },

    render({ input, width, height, time, variant }) {
      const isEffect = variant === 'ink';
      if (!instance || instanceVariant !== variant) {
        instance = new CogwheelInstance(isEffect);
        instanceVariant = variant;
        sheet.reset();
        hooks.newInstance?.(instance);
      }
      hooks.beforeFrame?.(instance);

      const resolved = instance.ProcessFrame(time);
      sheet.render(instance.steps, instance.runs, instance.geometry, instance.crank.Theta(), instance.crank.SlipTeeth(),
        resolved.render, null, [0, 0, width, height], isEffect ? input.texture : null, 1.0, 1.0);

      stats = {
        resolved,
        geometry: instance.geometry,
        layer: instance.crank.Layer(),
        phase: instance.crank.FigurePhase(),
        slip: instance.crank.SlipTeeth(),
        steps: instance.steps.size,
        frameSeconds: instance.clock.FrameSeconds(),
        unusable: instance.UnusableNote(resolved),
      };

      hooks.afterFrame?.(instance);
    },

    /**
     * The frame comparison's hook (AGENTS.md): a fresh instance and sheet,
     * `frames` frames at 60 fps exactly as cgtest --out drives the plugin
     * (SetTime( frame / 60 ), a target cleared to transparent black before
     * each), then the RGBA32F target read back and converted as cgtest's
     * toBytes converts it. `clip` true gives the effect cgtest's
     * makeTestClip. Returns base64 of top-down RGBA8.
     */
    offline({ effect = false, presetIndex = -1, sets = [], frames = 600, width = 640, height = 360, clip = false }) {
      const inst = new CogwheelInstance(effect);
      if (presetIndex >= 0) inst.SetFloatParameter(PT.PRESET, presetIndex);
      for (const [id, value] of sets) inst.SetFloatParameter(id, value);
      const own = new SheetRenderer(gl, quad, programs, floatLinear);

      const target = new PassBuffer(gl, { filter: 'nearest' });
      target.ensure(width, height, gl.RGBA32F);

      let clipTexture = null;
      if (clip) {
        const pixels = new Float32Array(width * height * 4);
        for (let y = 0; y < height; y += 1) {
          for (let x = 0; x < width; x += 1) {
            const u = f32(x / width);
            const v = f32(y / height);
            const o = (y * width + x) * 4;
            pixels[o] = u < 0.5 ? (v < 0.5 ? 0.85 : 0.15) : (v < 0.5 ? 0.15 : 0.75);
            pixels[o + 1] = u < 0.5 ? (v < 0.5 ? 0.20 : 0.70) : (v < 0.5 ? 0.55 : 0.75);
            pixels[o + 2] = u < 0.5 ? (v < 0.5 ? 0.20 : 0.25) : (v < 0.5 ? 0.80 : 0.20);
            pixels[o + 3] = 1.0;
          }
        }
        clipTexture = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, clipTexture);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, width, height, 0, gl.RGBA, gl.FLOAT, pixels);
        const filter = floatLinear ? gl.LINEAR : gl.NEAREST;
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.bindTexture(gl.TEXTURE_2D, null);
      }

      for (let frame = 0; frame < max2(1, frames); frame += 1) {
        const resolved = inst.ProcessFrame(frame / 60.0);
        target.clearTo(0, 0, 0, 0);
        own.render(inst.steps, inst.runs, inst.geometry, inst.crank.Theta(), inst.crank.SlipTeeth(),
          resolved.render, target.fbo, [0, 0, width, height], clipTexture, 1.0, 1.0);
      }

      gl.bindFramebuffer(gl.FRAMEBUFFER, target.fbo);
      const floats = new Float32Array(width * height * 4);
      gl.readPixels(0, 0, width, height, gl.RGBA, gl.FLOAT, floats);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);

      const bytes = new Uint8Array(width * height * 4);
      for (let y = 0; y < height; y += 1) {
        const src = (height - 1 - y) * width * 4;
        const dst = y * width * 4;
        for (let x = 0; x < width * 4; x += 1) bytes[dst + x] = lround(clamp(floats[src + x], 0, 1) * 255);
      }

      own.dispose();
      target.dispose();
      if (clipTexture) gl.deleteTexture(clipTexture);

      let binary = '';
      for (let i = 0; i < bytes.length; i += 0x8000) binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
      return btoa(binary);
    },
  };
}

//===========================================================================
// The parameters: the constructor's, in declaration order and groups, with the
// plugin's names and defaults. Export XML, Load XML and the About block are
// not here; the differences list says why.
//===========================================================================

/** Seed on this page: 1 to 99 of the plugin's 1 to 9,999. */
const kSeedChoices = 99;

const range = (lo, hi) => Array.from({ length: hi - lo + 1 }, (_, i) => String(lo + i));

/** id on the page, ParamId, hint. Types, names and ranges come from PARAM_INFO. */
const SPEC = [
  ['newSheet', PT.RESET, 'Wipes the paper and starts the stack again from its first figure. FF_TYPE_EVENT in the plugin: here a toggle the page lets go of after one frame.'],
  ['ringTeeth', PT.RING, 'Teeth on the fixed ring, 12 to 360. With Snap to Set on, the nearest ring the classic set has (96, 105, 144 or 150). The line under the picture says how many lobes that makes. FF_TYPE_INTEGER in the plugin; a dropdown of every value here.'],
  ['wheelTeeth', PT.WHEEL, 'Teeth on the wheel, 3 to 240. The figure closes after wheel ÷ gcd(ring, wheel) turns of the ring. With Snap to Set on, the nearest wheel the set has. FF_TYPE_INTEGER; a dropdown here.'],
  ['mesh', PT.MESH, 'Inside the ring (a hypotrochoid, the rosettes) or round the outside (an epitrochoid, the petals).'],
  ['penHole', PT.PEN, 'How far out the pen sits: 18% to 92% of the wheel’s radius. With Snap to Holes, the nearest of twelve modelled holes.'],
  ['snapToSet', PT.SNAP_SET, 'Holds Ring and Wheel to tooth counts from the classic set’s published part lists. Not checked against a set in hand.'],
  ['snapToHoles', PT.SNAP_HOLES, 'Holds the pen to one of twelve evenly spaced holes. The positions are a model; the discreteness is the point.'],
  ['sync', PT.SYNC, 'Free runs at Speed. The bar modes give a whole figure 1, 2, 4 or 8 bars. A browser has no host tempo, so here that is 120 BPM, the FFGL SDK’s default.'],
  ['speed', PT.RATE, 'Turns of the ring a second, 0.01 to 10, exponentially. Set by Sync in the bar modes.'],
  ['detail', PT.DETAIL, 'Pen steps per turn: 360, 1,440 or 5,760. The ink laid down is the same at any detail; only the corners change.'],
  ['seed', PT.SEED, 'Which wheels and holes a stack moves through, and which turns skip a tooth. FF_TYPE_INTEGER 1–9,999 in the plugin; 1–99 here.'],
  ['creep', PT.CREEP, 'A mesh a little out of true, 0 to 1 tooth of slip a turn. It precesses a closed figure, which is what keeps a drawing growing.'],
  ['skipChance', PT.SKIP, 'The chance, each turn of the ring, that a tooth jumps the mesh.'],
  ['skipSize', PT.SKIP_TEETH, 'How many teeth a jump goes, 1 to 12, either way. FF_TYPE_INTEGER; a dropdown here.'],
  ['layers', PT.LAYERS, 'Figures in a stack, 1 to 16. When one closes the pen lifts, moves, changes colour and draws the next on top. A stack of one never lifts. FF_TYPE_INTEGER; a dropdown here.'],
  ['onClosing', PT.CHANGE, 'What moves when a figure closes: the hole, the wheel, both, nothing (the same figure in the next pen), or Keep Going, under which a closure is not an event at all.'],
  ['unlessFaded', PT.UNLESS_FADED, 'A figure that took longer than the Fade time to come home carries on instead of closing, as under Keep Going.'],
  ['wipeSheet', PT.WIPE, 'A fresh sheet when the stack is complete.'],
  ['pens', PT.PEN_SET, 'One pen in the Ink colour, the box’s four, the larger six, a spectrum walked across the stack, or graphite.'],
  ['inkRed', PT.INK_R, 'The Ink colour, used when Pens is Ink Colour. FF_TYPE_RED, GREEN and BLUE: three sliders, as Arena shows them.'],
  ['inkGreen', PT.INK_G, 'The Ink colour’s green.'],
  ['inkBlue', PT.INK_B, 'The Ink colour’s blue.'],
  ['penType', PT.PEN_TYPE, 'Ballpoint lays ink per distance, the same darkness at any speed. Fibre Tip lays it per second, so it blooms where the pen slows down.'],
  ['blend', PT.BLEND, 'Multiply: transparent ink, so crossings darken (Beer’s law). Cover: opaque pigment that hides what is under it. Lift: an eraser in the pen hole.'],
  ['flow', PT.FLOW, 'How dark the line is. Multiplied by the nib inside the plugin, so it means darkness rather than ink per step.'],
  ['nib', PT.NIB, 'The nib’s width, as a share of the sheet’s height: a hairline to a marker.'],
  ['pressure', PT.SPREAD, 'How much the nib swells when it is pressed.'],
  ['inkFromClip', PT.INK_FROM_CLIP, 'SW Cogwheel Ink: the pen picks up the colour of the clip it is passing over. SW Cogwheel declares it and ignores it.'],
  ['paperRed', PT.PAPER_R, 'The paper. Changing it never spoils what is drawn: the sheet holds density, not colour.'],
  ['paperGreen', PT.PAPER_G, 'The paper’s green.'],
  ['paperBlue', PT.PAPER_B, 'The paper’s blue.'],
  ['paperFromClip', PT.PAPER_FROM_CLIP, 'SW Cogwheel Ink: the clip is the sheet the pen draws on. SW Cogwheel declares it and ignores it.'],
  ['clearPaper', PT.PAPER_CLEAR, 'No paper: the ink alone, with an alpha channel. Behind (below the picture) shows what it is over.'],
  ['grain', PT.GRAIN, 'How much of the paper’s tooth you can see.'],
  ['tooth', PT.TOOTH, 'How unevenly the paper takes ink, applied as the ink goes down.'],
  ['fade', PT.FADE, 'At 0 nothing fades, which is paper. Above it, the time for the drawing to fade to a third, 120 seconds down to 1.'],
  ['fadeByFigure', PT.FADE_FIGURES, 'Fade whole figures once they have closed, instead of the line as it is drawn.'],
  ['print', PT.PRINT, 'Negative: the drawing photographed and printed the other way up, the only route to a pale line on a dark ground.'],
  ['size', PT.ZOOM, 'The ring’s size against the sheet: at 1× the ring touches the top and bottom edges.'],
  ['centreX', PT.CENTRE_X, 'Where the ring’s centre sits across the sheet, ±1.5 paper units (the sheet is 2 tall).'],
  ['centreY', PT.CENTRE_Y, 'Where the ring’s centre sits up the sheet, ±1.5 paper units.'],
  ['showGears', PT.GEARS, 'Draws the ring, the wheel, the arm and the pen over the drawing.'],
  ['gearTintRed', PT.GEAR_R, 'The overlay’s colour.'],
  ['gearTintGreen', PT.GEAR_G, 'The overlay’s green.'],
  ['gearTintBlue', PT.GEAR_B, 'The overlay’s blue.'],
  ['mix', PT.MIX, 'SW Cogwheel calls it Opacity. SW Cogwheel Ink calls it Mix, and at 0 the clip comes back untouched.'],
  ['preset', PT.PRESET, 'The plugin’s eight factory drawings. Choosing one restarts the drawing on a clean sheet; moving a control a preset covers drops it back to Custom.'],
];

/** The kit's value (a dropdown index for an integer) to the host's. */
function toHost(p, value) {
  return p.ffType === 'integer' ? p.min + Math.round(value) : value;
}
function toKit(p, host) {
  if (p.ffType !== 'integer') return host;
  return clamp(Math.round(host) - p.min, 0, p.elements.length - 1);
}

const DEFAULTS = ConstructorDefaults();

/** The live state, for the readouts: the kit's values over the defaults. */
const live = { kit: null, instance: null, overInput: false };
function hostArray() {
  const p = Float32Array.from(DEFAULTS);
  if (live.kit) for (const q of PARAMS) p[q.slot] = toHost(q, live.kit.get(q.id));
  return p;
}
function readout(slot) {
  return () => {
    const crank = live.instance?.crank ?? (() => { const c = new Crank(); c.Restart(1); return c; })();
    return Display(slot, hostArray(), crank, live.overInput);
  };
}

const PARAMS = SPEC.map(([id, slot, hint]) => {
  const info = PARAM_INFO[slot];
  const p = { id, slot, name: NameOf(slot, false), group: GroupOf(slot), hint, ffType: info.type };
  if (info.type === 'integer') {
    const hi = slot === PT.SEED ? kSeedChoices : info.max;
    Object.assign(p, { type: 'option', min: info.min, elements: range(info.min, hi) });
  } else if (info.type === 'option') {
    Object.assign(p, { type: 'option', elements: info.elements });
  } else if (info.type === 'boolean' || info.type === 'event') {
    p.type = 'boolean';
  } else {
    // standard, and the colour channels: three sliders, as Arena shows them.
    Object.assign(p, { type: 'standard', display: readout(slot) });
  }
  p.default = toKit(p, DEFAULTS[slot]);
  return p;
});

//===========================================================================
// The page.
//===========================================================================

const query = new URLSearchParams(window.location.search);
const embed = query.has('embed') && query.get('embed') !== '0';
const initialVariant = query.get('plugin') === 'ink' ? 'ink' : 'source';
live.overInput = initialVariant === 'ink';

let renderer = null;

const mounted = mountDemo({
  name: 'Cogwheel',
  pluginId: 'CW01 · CW02',
  kind: ['source', 'effect'],
  tagline:
    'A Spirograph: a toothed ring pinned to the paper, a wheel rolling in mesh with it, and a pen in one of the wheel’s holes. Nothing evaluates a curve. Set the two tooth counts and the figure follows: 96 and 32 close in one turn with three lobes, 96 and 31 take thirty-one turns and have ninety-six. Ink is subtractive, so a second pen crossing the first darkens it the way it does on paper, and Creep, a mesh slightly out of true, is what keeps a drawing growing once it has closed. SW Cogwheel draws on its own paper; SW Cogwheel Ink can take your clip as the paper, or as the ink.',
  repo: 'https://github.com/stoatworks-labs/cogwheel',
  page: 'https://stoatworks-labs.com/software/cogwheel/',
  video: 'https://www.youtube.com/watch?v=_epebasO_is',

  blurb:
    'It is Cogwheel’s own GLSL (the ink, fade, compose and sheet passes), ported from the repository to WebGL2 unedited and fed by a JavaScript port of the plugin’s CPU half (the gear train, the crank, the pens and every control’s mapping), which a check in the repository holds to the plugin’s C++, step for step. The plugin has no audio input, so nothing is missing for want of one. SW Cogwheel reads no video; SW Cogwheel Ink runs on generated clips or your own.',

  showBackdrop: true,
  needFloat: true,

  variants: {
    label: 'Plugin',
    default: initialVariant,
    options: [
      { id: 'source', name: 'SW Cogwheel (source)', hint: 'CW01, FF_SOURCE: the drawing on its own paper. Reads no clip.' },
      { id: 'ink', name: 'SW Cogwheel Ink (effect)', hint: 'CW02, FF_EFFECT: the clip can be the paper (Paper from Clip) or the ink (Ink from Clip).' },
    ],
  },

  // For SW Cogwheel Ink only. Ink from Clip is, in the plugin's own words,
  // excellent on large areas of strong colour and mud on anything busy: the
  // bars are the first, the scene the second.
  sources: ['bars', 'scene', 'grid', 'ramp', 'spot', 'alpha'],

  params: PARAMS,

  differences: [
    'The CPU half is a PORT, not the plugin’s code. The plugin runs its machine on the CPU in double: Gears.cpp turns two tooth counts into a geometry, Crank.cpp turns the hand, slips and skips teeth and stacks the layers, Pens.cpp holds the pens and their absorptions, and Controls.cpp maps every control. Here all of that is JavaScript. A check in the repository runs the plugin’s own C++ and this port through one scenario (all eight presets for up to 900 frames, Keep Going held and let go, Unless Faded, Sync at two tempos, wheels on and off the set, a sixteen-layer spectrum with skipped teeth, a 149/150 train outside the ring at Fine and full speed, a train that cannot be threaded, and every control swept through its range for its mapping and its display string) and requires the two printouts to match. With the plugin’s own powf and logf results replayed into the port, all 1,204,856 lines are identical to the bit, every one of 1,168,470 pen steps included. On this page’s own Math two of those lines differ, each a float one or two units in the last place apart (a fade time and a size, both from powf). Four deliberately wrong ports, one of them wrong by a millionth of a tooth, are all caught.',
    'The frame logic and the GL calls are a port as well, and only a reader checks them line by line: the part of ProcessOpenGL that hands the sheet its arguments, and Sheet::Render’s passes, blends, per-run draws and uniforms. What checks them as a whole is a frame comparison in the repository: the plugin’s own harness (cgtest, the real plugin class on Apple’s OpenGL) and this page (headless Chrome on the same Mac, ANGLE on Metal) rendered at the same settings, 640 × 360, for 300 to 900 frames. In twelve cases (all eight presets, Fade by Figure, Clear Paper with Cover, and the effect with Ink from Clip and with Paper from Clip) every pixel came out within one level of 255 of the plugin’s, and 99.8% to 99.97% of them identical. A negative control with Speed moved from 0.737 to 0.740 fails it.',
    'The GPU half is not a port: the ink, fade, compose and sheet shaders are the plugin’s own GLSL, unedited, on the same 32-bit float sheet of optical density with the same blends. WebGL2 needs EXT_color_buffer_float to render into that sheet and EXT_float_blend to blend into it, and the page stops and says so without either rather than fall back to half-float, which stops darkening where a dense figure is darkest. Without OES_texture_float_linear the sheet is sampled NEAREST, which at its own size reads the same texels, and the line under the picture says so. demo/tools/check_shaders.py fails the repository’s verify script if a character of the shaders, or the way they are joined, drifts.',
    'The clock is the page’s, in seconds, so the plugin’s vote on whether its host sends seconds or milliseconds is not ported. Its clamp is: a frame is never taken as shorter than 1/240 s or longer than 1/24 s, so a browser drawing fewer than 24 frames a second draws more slowly than the plugin would rather than in bigger jumps. A redraw while paused is still a frame to the plugin, and adds 1/240 s of line. Restart is the clock jumping backwards, which the plugin carries on through, so the drawing carries on; New Sheet starts it again. Step is one frame of a sixtieth of a second.',
    'Nothing audio: the plugin has no audio input. Sync’s bar modes need the host’s tempo and a browser has none, so here they run at 120 BPM, which is the FFGL SDK’s own default when a host never sends one.',
    'Ring Teeth, Wheel Teeth, Skip Size and Layers are FF_TYPE_INTEGER in the plugin, with real ranges: here each is a dropdown of every value. Seed is FF_TYPE_INTEGER 1 to 9,999 in the plugin; here 1 to 99. The colours are three sliders each, as Arena shows them. New Sheet is FF_TYPE_EVENT: here a toggle the page lets go of after one frame. Export XML and Load XML write and read a file in the plugin’s Documents folder, which a page cannot, so they are not on the panel (Copy link carries a look instead). The About block is not on the panel.',
    'The Preset dropdown is the plugin’s own, with its applyPreset (the covered controls, a restart, a clean sheet) and its drop to Custom when a covered control moves. Its guard against a host restating values (hostIsRestatingItself) is not ported: Resolume pushes values back at the plugin, and this panel sends only what you move.',
    'Switching the Plugin dropdown is a new instance, so the drawing starts again on a clean sheet. The controls keep their values, as a composition moved between the two plugins does, since both declare the same list; Opacity becomes Mix. Ink from Clip and Paper from Clip are on both panels because both plugins declare them, and SW Cogwheel ignores them, as it does in Resolume.',
    'SW Cogwheel Ink runs on the kit’s generated clips or your own file, 8-bit and premultiplied as Resolume hands an effect its input. The frame comparison used cgtest’s own test clip, four flat colours in float.',
    'The line under the picture is the port’s own reading of the machine: the plugin’s display strings for Ring and Wheel, which figure of the stack is being drawn and how far round, and the slip. When a train cannot be threaded it shows the line the plugin writes to its log. It measures nothing.',
    'Nothing here is measured. The plugin’s harness checks, among sixteen invariants, that two pens crossing multiply (Beer’s law, off saturation), that the ink laid down is independent of Detail and of frame rate, that a closed figure stops and the defaults do not, and that Keep Going and Unless Faded are not events; a sweep fails if any control does nothing. That harness, not this page, is the reason to believe the drawing. The OpenFX build for Resolve has its own CPU renderer and is not on this page.',
  ],

  createRenderer: (gl, quad) => {
    renderer = createRenderer(gl, quad);
    return renderer;
  },
});

if (mounted) {
  const kit = mounted.params;
  live.kit = kit;

  //---------------------------------------------------------------------
  // The host's side of SetFloatParameter. A host sends the plugin what the
  // operator moved, in declaration order, and re-reads a control the plugin
  // raised an event for (a preset's values, the drop to Custom).
  //---------------------------------------------------------------------
  let sent = null;
  let syncing = false;

  const refresh = () => {
    syncing = true;
    kit.dispatchEvent(new CustomEvent('reset'));
    syncing = false;
  };

  function send(instance) {
    if (!sent) sent = Object.fromEntries(PARAMS.map((p) => [p.id, p.default]));
    for (const p of PARAMS) {
      const value = kit.get(p.id);
      if (value === sent[p.id]) continue;
      sent[p.id] = value;
      instance.SetFloatParameter(p.slot, toHost(p, value));
    }
    if (instance.raised.size === 0) return false;
    for (const p of PARAMS) {
      if (!instance.raised.has(p.slot)) continue;
      const value = toKit(p, instance.params[p.slot]);
      kit.set(p.id, value, { silent: true });
      sent[p.id] = kit.get(p.id);
    }
    instance.raised.clear();
    return true;
  }

  hooks.newInstance = (instance) => {
    live.instance = instance;
    live.overInput = instance.overInput;
    // A new instance starts at the constructor's values; the controls the
    // visitor set are sent to it, as a composition moved between the two
    // plugins carries them.
    sent = null;
  };
  // The panel is re-read whenever the plugin raised an event: a preset named
  // in the URL is applied on the first frame, after the panel was built.
  hooks.beforeFrame = (instance) => { if (send(instance)) refresh(); };
  hooks.afterFrame = () => {
    // A host lets go of an event; the kit has none, so the page does.
    if (kit.get('newSheet') > 0.5) kit.set('newSheet', 0);
  };

  kit.addEventListener('change', () => {
    if (syncing) return;
    if (renderer?.instance) {
      send(renderer.instance);
      refresh();
    } else if (!embed) {
      refresh();
    }
  });

  //---------------------------------------------------------------------
  // Which plugin: Mix or Opacity, and the clip controls in the transport.
  // By inline style, not the `hidden` attribute: kit.css gives these
  // elements a `display` of their own.
  //---------------------------------------------------------------------
  const show = (node, on) => { if (node) node.style.display = on ? '' : 'none'; };
  const labelOf = (id) => document.getElementById(`p-${id}`)?.closest('.prow')?.querySelector('.prow__name');

  function showForVariant(variant) {
    const effect = variant === 'ink';
    const mix = labelOf('mix');
    if (mix) mix.textContent = effect ? 'Mix' : 'Opacity';
    for (const field of document.querySelectorAll('.transport__field')) {
      if (field.querySelector('.transport__label')?.textContent === 'Clip') show(field, effect);
    }
    show(document.querySelector('.transport__file'), effect);
  }

  // Copy link carries the plugin.
  const toQuery = kit.toQuery.bind(kit);
  kit.toQuery = () => {
    const q = toQuery();
    if (mounted.state.variant === 'ink') q.set('plugin', 'ink');
    return q;
  };

  if (!embed) {
    showForVariant(mounted.state.variant);
    document.addEventListener('demo:state', () => showForVariant(mounted.state.variant));
    refresh();

    const stage = document.querySelector('.stage');
    if (stage) {
      //-------------------------------------------------------------------
      // A status line: what the port computed. It measures nothing.
      //-------------------------------------------------------------------
      const line = document.createElement('p');
      line.className = 'stage__status';
      line.id = 'cogwheel-stats';
      line.setAttribute('aria-live', 'off');
      stage.append(line);
      const note = document.createElement('p');
      note.className = 'stage__status';
      note.id = 'cogwheel-note';
      stage.append(note);

      setInterval(() => {
        const s = renderer?.stats;
        const instance = renderer?.instance;
        if (!s || !instance) return;
        const p = instance.params;
        const ring = Display(PT.RING, p, instance.crank, instance.overInput);
        const wheel = Display(PT.WHEEL, p, instance.crank, instance.overInput);
        const layers = s.resolved.crank.layers;
        line.textContent = s.unusable
          ? s.unusable
          : `Ring ${ring} · Wheel ${wheel}`
            + ` · figure ${s.layer + 1} of ${layers}, ${Math.round(100 * s.phase)}% round`
            + ` · slipped ${s.slip.toFixed(2)} teeth · ${s.steps.toLocaleString('en-GB')} pen steps this frame`;
        note.textContent =
          'The sheet is a 32-bit float buffer of optical density, blended as in the plugin.'
          + (renderer.floatLinear ? '' : ' This browser has no OES_texture_float_linear, so the sheet is sampled NEAREST, which at its own size reads the same texels.');
      }, 250);
    }
  }

  // For the headless checks in AGENTS.md: the page's pieces, read-only by convention.
  window.cogwheelDemo = { mounted, get renderer() { return renderer; }, PARAMS, PT, Display, Resolve, Solve };
}
