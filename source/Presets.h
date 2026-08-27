#pragma once

/**
    Factory presets: drawings an operator can reach in one gesture.

    Each entry is a recognisable *drawing* -- the three-lobed figure everyone
    who has held a Spirograph has made, the dense hundred-lobed web, the one
    where the gears are slipping -- rather than a random set of slider
    positions. Which is only possible because the gear train is two integers:
    a preset can name 96 and 52 and get the same figure every time, on any
    machine, at any resolution.

    The values live in the same parameter space both builds expose, so ONE
    table drives the FFGL and OFX plugins and a preset looks identical in
    Resolume and Resolve. Plain data only; the application machinery lives with
    each host's glue.

    **Standard parameters are the host-facing 0..1**, not engineering units --
    `Controls.cpp` maps them, and the comment beside each row says what the
    number means so the two cannot silently drift. **Integer and option
    parameters hold their real value**, because `SetParamInfo`'s 0..1 clamp is
    guarded by the parameter type and integers pass through it untouched.

    Element 0 of the host-facing dropdown is "Custom" and is not in this table:
    it means "the sliders are the truth".

    A preset does NOT cover: Sync (the FFGL build offers beat modes the OFX
    build has no tempo for, so an index here would mean different things in
    different hosts), Seed (which variation, not what kind), Centre and Mix
    (framing and level are the operator's), Ink From Clip and Paper From Clip
    (what the effect does to the footage is the operator's call), Gear Colour,
    and Reset.

    ## The first row is also the constructor's defaults

    `Classic Rosette` and the defaults in `Cogwheel.cpp` are the same drawing,
    written twice, and they have to be kept that way by hand -- `cgtest
    --defaults` is the test that fails when they go out of step. It is worth
    knowing why the fleet cares: escapement shipped a build whose defaults had
    been left behind by a retuned preset, so the plugin opened on a rig that
    saturated to white, and nothing in the test suite noticed because every
    test set its own parameters first.
*/

namespace cogwheel
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds this
/// order to its ParamIds and the OFX build to its param handles; both
/// static_assert against `kParamCount` so the three lists cannot drift apart
/// silently.
enum Param
{
	kRing,
	kWheel,
	kMesh,
	kPen,
	kSnapSet,
	kSnapHoles,
	kRate,
	kDetail,
	kCreep,
	kSkip,
	kSkipTeeth,
	kLayers,
	kChange,
	kWipe,
	kPenSet,
	kPenType,
	kInkR,
	kInkG,
	kInkB,
	kFlow,
	kNib,
	kSpread,
	kPaperR,
	kPaperG,
	kPaperB,
	kGrain,
	kTooth,
	kFade,
	kPrint,
	kZoom,
	kGears,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Cartridge. Warm, slightly grey, and emphatically not white: a sheet of pure
// white behind a coloured line is the single quickest way to make this look
// like a plot rather than a drawing.
#define COGWHEEL_CARTRIDGE 0.94f, 0.92f, 0.86f

inline constexpr Preset kPresets[] = {
	//   name                ring wheel mesh  pen     snapS snapH rate    det creep  skip   skipT lay chg wipe pen type inkR   inkG   inkB   flow    nib     spr    paper                grain  tooth   fade    prt zoom    gears
	{ "Classic Rosette", {     96,  52,   0, 0.770f,   1,    1,  0.737f,  1, 0.06f, 0.00f,   1,   4,  0,  1,   1,  0,  0.72f, 0.10f, 0.12f, 0.625f, 0.398f, 0.10f, COGWHEEL_CARTRIDGE, 0.15f, 0.222f, 0.000f,  0, 0.610f, 0.00f } },
	//96/52 mesh with a common factor of 4: thirteen turns of the ring, and
	//twenty-four lobes. The drawing everybody made first. Cranked at about a
	//turn and two thirds a second, so a figure takes eight seconds and the
	//stack of four is done in half a minute -- which is the longest a VJ will
	//sit through before deciding a plugin is broken.

	{ "Three Lobes",     {     96,  32,   0, 0.973f,   1,    1,  0.466f,  2, 0.50f, 0.00f,   1,   1,  3,  0,   4,  0,  0.34f, 0.34f, 0.36f, 0.699f, 0.287f, 0.05f, COGWHEEL_CARTRIDGE, 0.20f, 0.278f, 0.000f,  0, 0.610f, 0.00f } },
	//96/32 is a clean 3:1, so it closes in ONE turn and has three lobes. One
	//layer with the pen never lifted, so the slip is the whole show: half a
	//tooth a turn is five and a half degrees, and the deltoid walks round the
	//sheet filling it in as it goes. Turn Creep off and this preset stops dead
	//after one second, which is the point of it.

	{ "Dense Web",       {    105,  64,   0, 0.500f,   1,    1,  0.826f,  2, 0.02f, 0.00f,   1,   6,  2,  1,   3,  0,  0.72f, 0.10f, 0.12f, 0.534f, 0.199f, 0.05f, COGWHEEL_CARTRIDGE, 0.10f, 0.111f, 0.000f,  1, 0.610f, 0.00f } },
	//105 and 64 share no factor at all, so this closes only after 64 turns and
	//has 105 lobes. A light flow, because a hundred and five lobes cross each
	//other constantly and every crossing darkens. Printed as a negative, which
	//is the only way to a pale line on a dark ground -- see Controls.h.

	{ "Flower",          {     96,  45,   1, 0.838f,   1,    1,  0.667f,  1, 0.05f, 0.00f,   1,   6,  0,  1,   2,  0,  0.72f, 0.10f, 0.12f, 0.625f, 0.398f, 0.20f, COGWHEEL_CARTRIDGE, 0.15f, 0.222f, 0.000f,  0, 0.486f, 0.00f } },
	//Outside the ring: an epitrochoid, which is the petal shape rather than the
	//rosette. Six pens through six holes.

	{ "Slipping Gear",   {     96,  52,   0, 0.770f,   1,    0,  0.667f,  1, 0.45f, 0.30f,   3,   1,  3,  0,   0,  0,  0.55f, 0.09f, 0.10f, 0.593f, 0.398f, 0.30f, COGWHEEL_CARTRIDGE, 0.25f, 0.389f, 0.374f,  0, 0.610f, 0.00f } },
	//The classic ruined drawing, on purpose. A mesh well out of true plus a
	//three-tooth jump about once every three turns. The fade is here because
	//without it the sheet fills with wreckage in about a minute.

	{ "Wet Trail",       {     96,  33,   0, 0.700f,   1,    0,  0.900f,  2, 0.03f, 0.00f,   1,   1,  3,  0,   3,  1,  0.72f, 0.10f, 0.12f, 0.778f, 0.199f, 0.00f, COGWHEEL_CARTRIDGE, 0.08f, 0.111f, 0.855f,  0, 0.610f, 0.00f } },
	//Not a drawing: a trail. A fibre tip, cranked fast enough to get round all
	//eleven turns in a couple of seconds, against a two-second fade -- so the
	//whole figure is alive at once and the ink behind the pen is always
	//lifting. It is the one preset that is dishonest about the machine, because
	//a drawing does not fade, and it says so.

	{ "Chalkboard",      {    105,  40,   0, 0.635f,   1,    1,  0.566f,  1, 0.08f, 0.00f,   1,   3,  0,  1,   4,  1,  0.34f, 0.34f, 0.36f, 0.492f, 0.548f, 0.60f, COGWHEEL_CARTRIDGE, 0.55f, 0.667f, 0.000f,  1, 0.564f, 0.00f } },
	//Graphite, a heavy tooth and a wide soft nib, printed as a negative: chalk.
	//A fibre tip, because chalk gives up more of itself where the hand slows
	//down. The tooth is doing most of the work -- at 0.6 the sheet takes ink
	//unevenly enough that a single stroke has grain in it.

	{ "Show the Gears",  {    144,  60,   0, 0.770f,   1,    1,  0.520f,  1, 0.04f, 0.00f,   1,   4,  0,  1,   1,  0,  0.72f, 0.10f, 0.12f, 0.625f, 0.398f, 0.10f, COGWHEEL_CARTRIDGE, 0.15f, 0.222f, 0.000f,  0, 0.596f, 0.75f } },
	//The large ring, cranked slowly, with the ring, the wheel, the arm and the
	//pen drawn over the top. Five turns to close and twelve lobes, which at a
	//third of a turn a second is fifteen seconds a figure -- slow enough to
	//watch the wheel roll and fast enough that the drawing arrives. Put this on
	//a screen next to somebody who has never seen a Spirograph and they will
	//understand the plugin in about four seconds.
};

#undef COGWHEEL_CARTRIDGE

inline constexpr int kCount = static_cast< int >( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace cogwheel
