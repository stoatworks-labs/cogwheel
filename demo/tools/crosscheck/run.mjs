// The JavaScript half of the browser demo's crosscheck.
//
//     node demo/tools/crosscheck/run.mjs demo/tools/crosscheck/scenario.txt
//
// Slices the PORT block out of demo/plugin.js by its markers, runs it here
// with no DOM and no GL, and prints scenario.txt's printout in exactly the
// format printer.cpp prints it from the plugin's own C++. crosscheck.sh
// compares the two. Floats and doubles are printed as bit patterns, so a
// match is a match to the last bit.
//
//     node run.mjs scenario.txt [libm.txt]
//
// With libm.txt (libm_record.c's log of the plugin's own powf and logf calls)
// the port's powf and logf return the plugin's results instead of V8's; a call
// the log does not hold falls back to V8 and is counted on stderr.
//
// MUTANT=N runs the port with MUTANTS[ N ] applied to its text: a deliberately
// wrong port that crosscheck.sh requires to FAIL. A check that cannot fail is
// not a check. A mutant whose text is no longer in plugin.js exits 3.

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const viewL = new DataView(new ArrayBuffer(4));
const bitsOf = (v) => { viewL.setFloat32(0, v); return viewL.getUint32(0); };
const floatOf = (u) => { viewL.setUint32(0, u); return viewL.getFloat32(0); };

let libm;
const replay = { hits: 0, misses: 0 };
if (process.argv[3]) {
  const table = new Map();
  for (const line of readFileSync(process.argv[3], 'utf8').split('\n')) {
    const [name, x, y, r] = line.split(' ');
    if (name) table.set(`${name} ${x} ${y}`, parseInt(r, 16));
  }
  const key = (name, x, y) => `${name} ${bitsOf(x).toString(16).padStart(8, '0')} ${bitsOf(y).toString(16).padStart(8, '0')}`;
  const look = (k, fallback) => {
    const r = table.get(k);
    if (r === undefined) { replay.misses += 1; return fallback(); }
    replay.hits += 1;
    return floatOf(r);
  };
  libm = {
    powf: (x, y) => look(key('powf', x, y), () => Math.fround(Math.pow(x, y))),
    logf: (x) => look(key('logf', x, 0), () => Math.fround(Math.log(x))),
  };
}

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, '..', '..', 'plugin.js'), 'utf8');
const begin = source.indexOf('// ==== BEGIN PORT');
const end = source.indexOf('// ==== END PORT');
if (begin < 0 || end < 0 || end < begin) {
  console.error('run.mjs: the PORT markers are missing from demo/plugin.js');
  process.exit(2);
}

const MUTANTS = [
  // The crank: creep a millionth too strong.
  ['this.slipTeeth += creepPerStep;', 'this.slipTeeth += creepPerStep * 1.000001;'],
  // The mappings: Linear() in double, skipping the float rounding of (high - low) * t.
  ['return f32(low + f32(f32(high - low) * clamp(value, 0.0, 1.0)));', 'return f32(low + (high - low) * clamp(value, 0.0, 1.0));'],
  // The layer sequence: one bit of splitmix32's last multiplier.
  ['x = Math.imul(x ^ (x >>> 15), 0x735a2d97) >>> 0;', 'x = Math.imul(x ^ (x >>> 15), 0x735a2d96) >>> 0;'],
  // A display: the plugin prints Creep to two places.
  ['`${cfix(c.creepTeethPerTurn, 2)} teeth/turn`', '`${cfix(c.creepTeethPerTurn, 3)} teeth/turn`'],
];

let block = source.slice(begin, end);
if (process.env.MUTANT !== undefined) {
  const [from, to] = MUTANTS[Number(process.env.MUTANT)];
  if (!block.includes(from)) {
    console.error(`run.mjs: mutant ${process.env.MUTANT} no longer matches plugin.js: ${from}`);
    process.exit(3);
  }
  block = block.replace(from, to);
}

const EXPORTS = [
  'f32', 'lround', 'Solve', 'Crank', 'StepBlock', 'Clock', 'Resolve', 'Absorption', 'Display', 'PARAM_INFO',
  'PT', 'kSlots', 'kPresets', 'kPresetParamIDs', 'ConstructorDefaults',
];
// eslint-disable-next-line no-new-func
const port = new Function('libm', `${block}\nreturn { ${EXPORTS.join(', ')} };`)(libm);
const { f32, lround, Solve, Crank, StepBlock, Clock, Resolve, Absorption, Display, PARAM_INFO, PT, kSlots,
  kPresets, kPresetParamIDs, ConstructorDefaults } = port;

const view = new DataView(new ArrayBuffer(8));
const h32 = (v) => { view.setFloat32(0, v); return view.getUint32(0).toString(16).padStart(8, '0'); };
const h64 = (v) => { view.setFloat64(0, v); return view.getBigUint64(0).toString(16).padStart(16, '0'); };
const b = (v) => (v ? 1 : 0);

const out = [];
const print = (line) => out.push(line);

const seedOf = (p) => Math.max(1, lround(p[PT.SEED])) >>> 0;

const state = { effect: false, params: new Float32Array(kSlots), bpm: 120.0, crank: new Crank(), steps: new StepBlock(), runs: [] };

function reset(effect) {
  state.effect = effect;
  state.params = ConstructorDefaults();
  state.bpm = 120.0;
  state.crank = new Crank();
  state.crank.Restart(seedOf(state.params));
}

function geometry(g) {
  return ` g ${b(g.usable)} ${g.ringTeeth} ${g.wheelTeeth} ${g.lobes} ${g.turnsToClose} ${g.commonFactor} ${h64(g.wheelRadius)} ${h64(g.orbit)} ${h64(g.penRadius)} ${h64(g.spinPerOrbit)} ${h64(g.penPhase)} ${h64(g.outerRadius)} ${h64(g.innerRadius)}`;
}

function resolve(r) {
  const c = r.crank;
  let line = `V crank ${c.train.ringTeeth} ${c.train.wheelTeeth} ${c.train.mesh} ${h64(c.train.penFraction)} ${b(c.snapWheelToSet)} ${b(c.snapPenToHoles)} ${c.sync} ${h64(c.turnsPerSecond)} ${h64(c.bpm)} ${h64(c.creepTeethPerTurn)} ${h64(c.skipChancePerTurn)} ${h64(c.skipTeeth)} ${c.layers} ${c.change} ${b(c.wipeOnRestack)} ${h64(c.closeWithinSeconds)} ${c.seed} ${c.stepsPerTurn} ${c.paletteCount} |`;
  const p = r.render;
  line += ` render ${h32(p.flow)} ${b(p.perDistance)} ${h32(p.nibSigma)} ${h32(p.nibSpread)} ${h32(p.densityFloor)} ${h32(p.paperColour[0])} ${h32(p.paperColour[1])} ${h32(p.paperColour[2])} ${b(p.paperFromClip)} ${h32(p.paperGrain)} ${h32(p.tooth)} ${h32(p.toothScale)} ${h32(p.fadeSeconds)} ${b(p.fadeByFigure)} ${b(p.negative)} ${b(p.clearPaper)} ${h32(p.scale)} ${h32(p.centre[0])} ${h32(p.centre[1])} ${h32(p.gearLevel)} ${b(p.inkFromClip)} ${p.blend}`;
  line += ` ${h32(p.gearColour[0])} ${h32(p.gearColour[1])} ${h32(p.gearColour[2])} ${h32(p.opacity)} ${h32(p.passthrough)} ${h32(p.frameSeconds)} |`;
  line += ` detail ${r.detail} pens ${r.penSet} palette`;
  for (let i = 0; i < r.paletteCount; i += 1) {
    const colour = [r.palette[i * 3], r.palette[i * 3 + 1], r.palette[i * 3 + 2]];
    const absorb = Absorption(colour);
    line += ` ${h32(colour[0])},${h32(colour[1])},${h32(colour[2])}/${h32(absorb[0])},${h32(absorb[1])},${h32(absorb[2])}`;
  }
  print(line);
}

let frameNumber = 0;
function frames(count, seconds) {
  for (let f = 0; f < count; f += 1) {
    const resolved = Resolve(state.params, state.bpm, state.effect);
    const g = state.crank.Advance(resolved.crank, seconds, state.steps, state.runs);
    const wipe = state.crank.WipeRequested();
    if (wipe) state.crank.ClearWipeRequest();
    const c = state.crank;
    print(`F ${frameNumber++}${geometry(g)} c ${c.Layer()} ${c.FiguresClosed()} ${b(wipe)} ${h64(c.Theta())} ${h64(c.SlipTeeth())} ${h64(c.FigurePhase())} ${h64(c.FigureSeconds())} ${state.steps.size} ${state.runs.length}`);
    for (const run of state.runs) {
      print(`R ${run.first} ${run.count} ${h32(run.colour[0])} ${h32(run.colour[1])} ${h32(run.colour[2])} ${run.layer} ${b(run.closes)}`);
    }
    const d = state.steps.data;
    for (let i = 0; i < state.steps.size; i += 1) {
      print(`S ${h32(d[i * 4])} ${h32(d[i * 4 + 1])} ${h32(d[i * 4 + 2])} ${h32(d[i * 4 + 3])}`);
    }
  }
}

const swept = (id) => id !== PT.RESET && id !== PT.PRESET && id !== PT.EXPORT && id !== PT.LOAD && id < kSlots;

function sweepValues(id, full) {
  const info = PARAM_INFO[id];
  const values = [];
  if (info.type === 'integer') {
    if (info.max - info.min > 1000) {
      for (let v = info.min; v <= info.min + 30; v += 1) values.push(v);
      values.push(info.max);
    } else {
      for (let v = info.min; v <= info.max; v += 1) values.push(v);
    }
  } else if (info.type === 'option') {
    for (let v = 0; v < info.elements.length; v += 1) values.push(v);
  } else if (info.type === 'boolean') {
    values.push(0, 1);
  } else {
    const steps = full ? 100 : 20;
    for (let k = 0; k <= steps; k += 1) values.push(f32(k / steps));
  }
  return values;
}

function displays(full) {
  // What printer.cpp's plugin holds: the scenario's params[], with Preset at
  // Custom. Its crank is a fresh one, restarted on Seed 1 by the constructor.
  const p = Float32Array.from(state.params);
  p[PT.PRESET] = 0;
  const crank = new Crank();
  crank.Restart(1);
  for (let id = 0; id < kSlots; id += 1) {
    if (!swept(id)) continue;
    const kept = p[id];
    for (const v of sweepValues(id, full)) {
      p[id] = v;
      let line = `D ${id} ${h32(f32(v))} `;
      for (let shown = 0; shown < kSlots; shown += 1) {
        if (!swept(shown)) continue;
        line += `${Display(shown, p, crank, state.effect)}|`;
      }
      print(line);
      if (full) resolve(Resolve(p, state.bpm, state.effect));
    }
    p[id] = kept;
  }
}

function clock(times) {
  const c = new Clock();
  for (const t of times) {
    c.Update(t);
    print(`K ${h64(t)} ${h64(c.FrameSeconds())} ${h64(c.Now())}`);
  }
}

const scenario = readFileSync(process.argv[2] ?? join(here, 'scenario.txt'), 'utf8');
for (const raw of scenario.split('\n')) {
  if (raw === '' || raw.startsWith('#')) continue;
  const words = raw.trim().split(/\s+/);
  const verb = words[0];
  if (verb === 'scenario') {
    print(`== ${words[1]}`);
    frameNumber = 0;
  } else if (verb === 'reset') {
    reset(Number(words[1]) !== 0);
  } else if (verb === 'preset') {
    const row = kPresets[Number(words[1]) - 1];
    for (let j = 0; j < kPresetParamIDs.length; j += 1) state.params[kPresetParamIDs[j]] = row.v[j];
  } else if (verb === 'set') {
    state.params[Number(words[1])] = f32(Number(words[2]));
  } else if (verb === 'bpm') {
    state.bpm = Number(words[1]);
  } else if (verb === 'restart') {
    state.crank.Restart(seedOf(state.params));
  } else if (verb === 'frames') {
    frames(Number(words[1]), Number(words[2]));
  } else if (verb === 'resolve') {
    resolve(Resolve(state.params, state.bpm, state.effect));
  } else if (verb === 'displays') {
    displays(Number(words[1]) !== 0);
  } else if (verb === 'clock') {
    clock(words.slice(1).map(Number));
  } else {
    console.error(`run.mjs: unknown verb '${verb}'`);
    process.exit(2);
  }
}

process.stdout.write(`${out.join('\n')}\n`);
if (libm) console.error(`libm replay: ${replay.hits} calls replayed, ${replay.misses} not in the plugin's log`);
