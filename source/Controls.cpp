#include "Controls.h"

#include "machine/Wheels.h"

#include <algorithm>
#include <cmath>

namespace cogwheel
{

const char* const kMeshNames[ kMeshCount ] = { "Inside the Ring", "Outside the Ring" };

const char* const kSyncNames[ kSyncCount ] = {
	"Free", "1 Bar", "2 Bars", "4 Bars", "8 Bars"
};

const char* const kDetailNames[ kDetailCount ] = { "Draft", "Normal", "Fine" };

const char* const kChangeNames[ kChangeCount ] = {
	"Next Hole", "Next Wheel", "Wheel and Hole", "Same Figure"
};

const char* const kPenSetNames[ kPenSetCount ] = {
	"Ink Colour", "Four Pens", "Six Pens", "Spectrum", "Graphite"
};

const char* const kPrintNames[ kPrintCount ] = { "Positive", "Negative" };

const char* const kPenTypeNames[ kPenTypeCount ] = { "Ballpoint", "Fibre Tip" };

int Option( float value, int count )
{
	//An option parameter holds its element *value*, not a 0..1 fraction, so it
	//is rounded rather than scaled. Getting this backwards gives a dropdown
	//permanently stuck on its first entry.
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

float Exponential( float value, float low, float high )
{
	const float t = std::clamp( value, 0.0f, 1.0f );
	return low * std::pow( high / low, t );
}

float Linear( float value, float low, float high )
{
	return low + ( high - low ) * std::clamp( value, 0.0f, 1.0f );
}

int StepsPerTurn( Detail detail )
{
	switch( detail )
	{
		case Detail::Draft: return 360;
		case Detail::Fine:  return 5760;
		case Detail::Normal:
		default:            return 1440;
	}
}

namespace
{
inline bool boolean( float v )
{
	return v > 0.5f;
}

inline int integer( float v )
{
	return static_cast< int >( std::lround( v ) );
}
} // namespace

Resolved Resolve( const float* p, double bpm, bool overInput )
{
	Resolved r;

	// -- The gear train ------------------------------------------------------
	r.crank.train.ringTeeth  = std::clamp( integer( p[ PT_RING ] ), 12, 360 );
	r.crank.train.wheelTeeth = std::clamp( integer( p[ PT_WHEEL ] ), 3, 240 );
	r.crank.train.mesh       = static_cast< Mesh >( Option( p[ PT_MESH ], kMeshCount ) );

	//The pen never reaches the rim and never reaches the axle: at the rim it
	//would be on the teeth and at the axle it would draw a circle. The range is
	//the same one the modelled holes span, so Snap to Holes is a restriction of
	//the slider rather than a different control.
	r.crank.train.penFraction = Linear( p[ PT_PEN ],
	                                    static_cast< float >( kInnermostHole ),
	                                    static_cast< float >( kOutermostHole ) );

	r.crank.snapWheelToSet = boolean( p[ PT_SNAP_SET ] );
	r.crank.snapPenToHoles = boolean( p[ PT_SNAP_HOLES ] );

	// -- The crank -----------------------------------------------------------
	r.crank.sync           = static_cast< Sync >( Option( p[ PT_SYNC ], kSyncCount ) );
	r.crank.turnsPerSecond = Exponential( p[ PT_RATE ], 0.01f, 10.0f );
	r.crank.bpm            = bpm;
	r.crank.seed           = static_cast< uint32_t >( std::max( 1, integer( p[ PT_SEED ] ) ) );

	r.detail             = static_cast< Detail >( Option( p[ PT_DETAIL ], kDetailCount ) );
	r.crank.stepsPerTurn = StepsPerTurn( r.detail );

	// -- Slip ----------------------------------------------------------------
	//
	// Teeth per turn of the ring. A tenth of a tooth per turn on a 52-tooth
	// wheel is 0.7 degrees of precession a turn, which over a thirteen-turn
	// figure is nine degrees -- plainly visible, and still recognisably the
	// same drawing. A whole tooth a turn is a mesh nobody would keep using.
	r.crank.creepTeethPerTurn = Linear( p[ PT_CREEP ], 0.0f, 1.0f );
	r.crank.skipChancePerTurn = Linear( p[ PT_SKIP ], 0.0f, 1.0f );
	r.crank.skipTeeth         = static_cast< double >( std::clamp( integer( p[ PT_SKIP_TEETH ] ), 1, 12 ) );

	// -- Layers --------------------------------------------------------------
	r.crank.layers        = std::clamp( integer( p[ PT_LAYERS ] ), 1, kMaxLayers );
	r.crank.change        = static_cast< Change >( Option( p[ PT_CHANGE ], kChangeCount ) );
	r.crank.wipeOnRestack = boolean( p[ PT_WIPE ] );

	// -- Framing -------------------------------------------------------------
	//
	// Resolved BEFORE the pen, because Flow's mapping needs the scale: see the
	// note there. The ring's pitch radius is 1 paper unit and the sheet is 2
	// units tall, so a scale of 1 puts the ring exactly on the top and bottom
	// edges. The default sits just inside that.
	r.render.scale       = Exponential( p[ PT_ZOOM ], 0.1f, 4.0f );
	//Plus or minus one and a half paper units, where the sheet is two tall. A
	//ring pushed a full sheet-height off centre still has part of the figure on
	//the paper, and that is the point of the number: at the plus-or-minus two
	//this used to be, BOTH ends of Centre Y put the whole drawing off the
	//bottom or the top and the control was, correctly and uselessly, identical
	//at each end. `tools/sweep.py` reported it as dead, which is exactly what
	//that tool is for.
	r.render.centre[ 0 ] = Linear( p[ PT_CENTRE_X ], -1.5f, 1.5f );
	r.render.centre[ 1 ] = Linear( p[ PT_CENTRE_Y ], -1.5f, 1.5f );

	// -- The pen -------------------------------------------------------------
	r.penSet = static_cast< PenSet >( Option( p[ PT_PEN_SET ], kPenSetCount ) );

	const float ink[ 3 ] = { std::clamp( p[ PT_INK_R ], 0.0f, 1.0f ),
	                         std::clamp( p[ PT_INK_G ], 0.0f, 1.0f ),
	                         std::clamp( p[ PT_INK_B ], 0.0f, 1.0f ) };

	r.paletteCount = BuildPalette( r.penSet, r.crank.layers, ink, r.palette, kMaxLayers );
	r.crank.palette      = r.palette;
	r.crank.paletteCount = r.paletteCount;

	//Nib sigma in paper units, where 1 unit is half the sheet's height. The
	//floor is a nib narrower than an output pixel at 1080p, which is a hairline;
	//the ceiling is a marker.
	r.render.nibSigma  = Exponential( p[ PT_NIB ], 0.0004f, 0.04f );
	r.render.nibSpread = Linear( p[ PT_SPREAD ], 0.0f, 1.5f );

	//-----------------------------------------------------------------------
	// Flow, and why it is multiplied by the nib.
	//
	// The renderer's `flow` is ink per quantum and the peak density of a line
	// therefore goes as `flow / sigma`: the same ink spread across a wider line
	// is a paler line. Left like that, Nib would be a darkness control with a
	// width side effect, and every preset would have to be retuned the moment
	// anybody touched it.
	//
	// A real pen does not behave that way either. A broader nib delivers
	// proportionally more ink -- that is what makes it broader rather than
	// merely blurrier -- so multiplying by the nib width is the physical answer
	// as well as the usable one, and it makes Flow mean DARKNESS, independent
	// of width.
	//
	// The `scale` factor does the same job for the framing. The shader works in
	// scaled coordinates, so a stroke's length and the nib's sigma both carry
	// the operator's Size; multiplying here cancels it exactly for a ballpoint,
	// and leaves a fibre tip's darkness depending on Size, which is correct --
	// a bigger figure at the same crank means a faster nib and a thinner line.
	//-----------------------------------------------------------------------
	r.render.perDistance =
		Option( p[ PT_PEN_TYPE ], kPenTypeCount ) == static_cast< int >( PenType::Ballpoint );

	const float darkness = Exponential( p[ PT_FLOW ], 0.05f, 50.0f );
	r.render.flow        = darkness * r.render.nibSigma;
	if( r.render.perDistance )
	{
		r.render.flow *= r.render.scale;
	}
	else
	{
		//The two pens are in different units -- one per length, one per second
		//-- so a single Flow slider cannot mean the same thing in both without
		//a speed to convert between them. This is that speed, in paper units
		//per second, and it is a CALIBRATION rather than physics: it is chosen
		//so that switching pen type does not send the operator hunting up and
		//down the Flow slider to find the drawing again. Nothing downstream
		//depends on its value.
		constexpr float kFibreReferenceSpeed = 4.0f;
		r.render.flow *= kFibreReferenceSpeed;
	}

	//Both clip controls are the effect build's alone. Reading a texture that is
	//not bound would give the source build a pen that draws in black and a
	//sheet that is black, which looks like a bug rather than like a control in
	//the wrong place.
	r.render.inkFromClip   = overInput && boolean( p[ PT_INK_FROM_CLIP ] );
	r.render.paperFromClip = overInput && boolean( p[ PT_PAPER_FROM_CLIP ] );

	// -- The paper -----------------------------------------------------------
	r.render.paperColour[ 0 ] = std::clamp( p[ PT_PAPER_R ], 0.0f, 1.0f );
	r.render.paperColour[ 1 ] = std::clamp( p[ PT_PAPER_G ], 0.0f, 1.0f );
	r.render.paperColour[ 2 ] = std::clamp( p[ PT_PAPER_B ], 0.0f, 1.0f );
	r.render.paperGrain       = Linear( p[ PT_GRAIN ], 0.0f, 1.0f );
	r.render.tooth            = Linear( p[ PT_TOOTH ], 0.0f, 0.9f );
	r.render.toothScale       = 220.0f;

	//Zero is off, and off means paper: a drawing does not fade. Above zero the
	//range runs from a couple of seconds -- which is a trail rather than a
	//drawing -- to two minutes, by which point a whole stack of layers has been
	//laid down before the first one has gone.
	r.render.fadeSeconds = p[ PT_FADE ] <= 0.001f ? 0.0f
	                                              : Exponential( p[ PT_FADE ], 120.0f, 1.0f );

	r.render.negative = Option( p[ PT_PRINT ], kPrintCount ) == static_cast< int >( Print::Negative );

	// -- The overlay ---------------------------------------------------------
	r.render.gearLevel       = std::clamp( p[ PT_GEARS ], 0.0f, 1.0f );
	r.render.gearColour[ 0 ] = std::clamp( p[ PT_GEAR_R ], 0.0f, 1.0f );
	r.render.gearColour[ 1 ] = std::clamp( p[ PT_GEAR_G ], 0.0f, 1.0f );
	r.render.gearColour[ 2 ] = std::clamp( p[ PT_GEAR_B ], 0.0f, 1.0f );

	// -- Output --------------------------------------------------------------
	//
	// Mix is a passthrough fraction on the effect build and an opacity on the
	// source, because a source has nothing to mix with. Splitting it into two
	// controls would mean a composition could not be moved between the two
	// builds without the numbering shifting.
	const float mix = std::clamp( p[ PT_MIX ], 0.0f, 1.0f );
	if( overInput )
	{
		r.render.passthrough = 1.0f - mix;
		r.render.opacity     = 1.0f;
	}
	else
	{
		r.render.passthrough = 0.0f;
		r.render.opacity     = mix;
	}

	return r;
}

} // namespace cogwheel
