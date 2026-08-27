#pragma once

#include "machine/Crank.h"
#include "machine/Gears.h"
#include "machine/Pens.h"
#include "render/Sheet.h"

namespace cogwheel
{
/**
	Parameter ids.

	**The declaration order in Cogwheel.cpp is the order the host shows them,
	and `SetParamGroup` collapses *runs* of consecutive ids into one group.** So
	reordering this enum does not merely rearrange the panel -- it silently
	splits a group in two, or merges two into one, and the result renders as a
	duplicated group header that reads as a bug.

	**Renumbering a released id is safe; RENAMING one is not.** A Resolume
	composition stores `name`/`value` pairs, and only for the parameters that
	differ from their default -- measured on Arena 7.27.1 for `vectrix`. So a
	control may be moved to where it belongs in the panel, but a renamed control
	silently loses its saved value, and since only non-defaults are written
	there is nothing left in the file to notice it by.

	The append-only rule still holds for an OPTION's ELEMENTS, which are stored
	as numbers. Add a new mode at the end; never insert one.
*/
enum ParamId : unsigned int
{
	// -- The gear train ------------------------------------------------------
	PT_RING = 0,
	PT_WHEEL,
	PT_MESH,
	PT_PEN,
	PT_SNAP_SET,
	PT_SNAP_HOLES,

	// -- The crank -----------------------------------------------------------
	PT_SYNC,
	PT_RATE,
	PT_DETAIL,
	PT_RESET,
	PT_SEED,

	// -- Slip ----------------------------------------------------------------
	PT_CREEP,
	PT_SKIP,
	PT_SKIP_TEETH,

	// -- Layers --------------------------------------------------------------
	PT_LAYERS,
	PT_CHANGE,
	PT_WIPE,

	// -- The pen -------------------------------------------------------------
	PT_PEN_SET,
	PT_INK_R,
	PT_INK_G,
	PT_INK_B,
	PT_PEN_TYPE,
	PT_FLOW,
	PT_NIB,
	PT_SPREAD,
	PT_INK_FROM_CLIP,

	// -- The paper -----------------------------------------------------------
	PT_PAPER_R,
	PT_PAPER_G,
	PT_PAPER_B,
	PT_PAPER_FROM_CLIP,
	PT_GRAIN,
	PT_TOOTH,
	PT_FADE,
	PT_PRINT,

	// -- Framing -------------------------------------------------------------
	PT_ZOOM,
	PT_CENTRE_X,
	PT_CENTRE_Y,

	// -- The overlay ---------------------------------------------------------
	PT_GEARS,
	PT_GEAR_R,
	PT_GEAR_G,
	PT_GEAR_B,

	// -- Output --------------------------------------------------------------
	PT_MIX,

	// -- Preset --------------------------------------------------------------
	//
	// Last, and its own group, so it is where every other plugin in the fleet
	// puts it, and declared AFTER the real controls so that adding a preset
	// cannot shift a saved composition's parameter numbering.
	PT_PRESET,

	// -- The Stoatworks About block ------------------------------------------
	//
	// One display-only text line followed by one button per link. How many
	// buttons there are is decided by which URLs the branding header carries,
	// so Cogwheel.cpp static_asserts this run against `about::kParamCount` --
	// adding a URL later would otherwise silently shift PT_COUNT and leave the
	// last button undeclared.
	PT_ABOUT_TEXT,
	PT_ABOUT_BUTTON_1,
	PT_ABOUT_BUTTON_2,
	PT_ABOUT_BUTTON_3,
	PT_ABOUT_BUTTON_4,

	PT_COUNT
};

/// How a pen gives up its ink.
///
/// Not decoration -- it is the difference between two real classes of pen, and
/// it changes what the whole renderer is doing with `dt`.
///
/// A **ballpoint** delivers ink by rolling: the ball turns as it travels, so
/// the amount laid down is proportional to the DISTANCE covered and a line is
/// the same darkness however fast the hand moved. That is what came in the
/// Spirograph box, and it is the default for that reason.
///
/// A **fibre tip** feeds by capillary action, which is proportional to TIME.
/// Move slowly and more ink arrives in the same length of line, so the cusps of
/// a figure -- where the pen is barely moving -- bloom and the fast parts go
/// thin. It is the more beautiful of the two and the less faithful.
///
/// Both go through exactly the same closed form in the ink shader; the only
/// difference is whether a step's quantum is `Flow * dt` or `Flow * length`.
/// Neither computes a speed, and neither divides by one.
enum class PenType : int
{
	Ballpoint = 0,
	FibreTip,
	Count
};

/// What is shown: the drawing, or a photographic negative of it.
///
/// Ink can only darken paper -- that is what Beer's law says and what a pen
/// does -- so a pale line on a dark sheet is not something the machine can
/// make. A negative is: you photograph the drawing and print it the other way
/// up. It is the honest route to light-on-dark and it costs one subtraction.
enum class Print : int
{
	Positive = 0,
	Negative,
	Count
};

/// How finely the pen path is walked.
enum class Detail : int
{
	Draft = 0, ///< 360 steps a turn. Visibly polygonal on a big figure.
	Normal,    ///< 1440.
	Fine,      ///< 5760.
	Count
};

/// Option-parameter element counts, so the declaration and the reader cannot
/// disagree about how many entries a dropdown has.
constexpr int kMeshCount   = static_cast< int >( Mesh::Count );
constexpr int kSyncCount   = static_cast< int >( Sync::Count );
constexpr int kDetailCount = static_cast< int >( Detail::Count );
constexpr int kChangeCount = static_cast< int >( Change::Count );
constexpr int kPenSetCount = static_cast< int >( PenSet::Count );
constexpr int kPrintCount  = static_cast< int >( Print::Count );
constexpr int kPenTypeCount = static_cast< int >( PenType::Count );

extern const char* const kMeshNames[ kMeshCount ];
extern const char* const kSyncNames[ kSyncCount ];
extern const char* const kDetailNames[ kDetailCount ];
extern const char* const kChangeNames[ kChangeCount ];
extern const char* const kPenSetNames[ kPenSetCount ];
extern const char* const kPrintNames[ kPrintCount ];
extern const char* const kPenTypeNames[ kPenTypeCount ];

/// The most layers a stack may hold, and therefore the size of the palette
/// buffer that is filled once per frame rather than allocated.
constexpr int kMaxLayers = 16;

//---------------------------------------------------------------------------
// 0..1 to engineering units.
//
// Every continuous host parameter is 0..1 and mapped here. That is not a
// stylistic choice: `CFFGLPluginManager::SetParamInfo` clamps a default into
// 0..1 *before* returning, and `SetParamRange` can only be called afterwards
// because it looks the parameter up by id. So a STANDARD parameter declared in
// turns per second cannot declare a default in turns per second -- 0.5 would
// survive and 4 would silently become 1.
//
// Integer-typed parameters are the documented exception: the clamp is guarded
// by `if( pType == FF_TYPE_STANDARD )`, so FF_TYPE_INTEGER defaults pass
// through untouched and SetParamRange then widens them. Tooth counts use that,
// which is the whole reason they can be tooth counts.
//---------------------------------------------------------------------------

/// Read an option parameter's element value as an index.
int Option( float value, int count );

/// Exponential map, for anything whose useful range spans decades and where a
/// linear slider would spend nine tenths of its travel in the top octave.
float Exponential( float value, float low, float high );

/// Linear map.
float Linear( float value, float low, float high );

/// Steps the pen path is walked at, per turn of the ring.
int StepsPerTurn( Detail detail );

/// Everything both hosts need, assembled from the raw 0..1 array.
///
/// One struct and one function, shared by the FFGL and OFX builds, so the two
/// cannot disagree about what a slider means. The palette is carried inline
/// rather than allocated because `Resolve` runs every frame.
struct Resolved
{
	CrankParams crank;
	Sheet::RenderParams render;
	Detail detail = Detail::Normal;
	PenSet penSet = PenSet::Classic;
	float palette[ kMaxLayers * 3 ] = {};
	int paletteCount = 0;
};

/// `overInput` is the effect build: it is what decides whether the clip is
/// allowed to be the ink or the paper, so that the source build ignores those
/// two controls rather than silently reading a texture that is not there.
Resolved Resolve( const float* params, double bpm, bool overInput );

} // namespace cogwheel
