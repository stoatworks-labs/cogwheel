#include "Shaders.h"

namespace cogwheel::shaders
{
namespace
{

// ---------------------------------------------------------------------------
// Constants, shared by both stages so they cannot drift.
// ---------------------------------------------------------------------------
//
// `Extent` is the half-width of the quad the ink pass draws around each
// segment, in units of the nib's sigma. 4.5 sigma keeps 99.9993% of a Gaussian,
// and the fragment stage subtracts the profile's value at exactly this distance
// so that what is thrown away goes smoothly rather than as a step. Two stages
// have to agree about the number -- the vertex one sizing the quad and the
// fragment one subtracting the pedestal -- which is why it is here.
const char* const kConstants = R"(#version 410 core

const float Extent     = 4.5;
const float Sqrt2Pi    = 2.50662827463100050;
const float InvSqrt2Pi = 0.39894228040143268;
const float Pi         = 3.14159265358979324;
const float TwoPi      = 6.28318530717958648;
)";

// ---------------------------------------------------------------------------
// Shared fragment helpers.
// ---------------------------------------------------------------------------
const char* const kFragmentHelpers = R"(
//---------------------------------------------------------------------------
// The standard normal CDF.
//
// GLSL has no erf, so this is the usual tanh approximation, good to about 3e-4.
// The clamp is not tidiness: a driver computing tanh as (e^2x - 1)/(e^2x + 1)
// overflows to inf around x = 44 and then produces inf/inf = NaN -- for
// arguments a stationary pen generates constantly. One NaN in the paper buffer
// survives for the life of the drawing, because NaN times a fade is NaN.
//---------------------------------------------------------------------------
float ncdf( float x )
{
	float t = clamp( 0.7978845608 * ( x + 0.044715 * x * x * x ), -8.0, 8.0 );
	return 0.5 * ( 1.0 + tanh( t ) );
}

//---------------------------------------------------------------------------
// The paper's tooth.
//
// Cartridge paper is not flat: it is a mat of fibres, and a nib dragged across
// it deposits more where a fibre stands proud and less in the hollows. Two
// octaves of value noise, evaluated in PAPER coordinates rather than pixels so
// that the grain is a property of the sheet and does not change size when the
// composition resolution does.
//
// It is applied at DEPOSIT, not at readout. That is the difference between
// paper that takes ink unevenly -- where a second stroke over the same fibre
// also takes less -- and a grey texture laid over the finished drawing.
//---------------------------------------------------------------------------
float hash21( vec2 p )
{
	//The classic sin-fract hash. Its artefacts are its own lattice, which at
	//two octaves and this amplitude is well under the grain it is modelling.
	return fract( sin( dot( p, vec2( 127.1, 311.7 ) ) ) * 43758.5453123 );
}

float valueNoise( vec2 p )
{
	vec2 i = floor( p );
	vec2 f = fract( p );
	vec2 u = f * f * ( 3.0 - 2.0 * f );
	return mix( mix( hash21( i + vec2( 0.0, 0.0 ) ), hash21( i + vec2( 1.0, 0.0 ) ), u.x ),
	            mix( hash21( i + vec2( 0.0, 1.0 ) ), hash21( i + vec2( 1.0, 1.0 ) ), u.x ), u.y );
}

float paperTooth( vec2 paper, float amount, float scale )
{
	if( amount <= 0.0 )
		return 1.0;

	float n = 0.65 * valueNoise( paper * scale )
	        + 0.35 * valueNoise( paper * scale * 2.7 + vec2( 17.0, 5.0 ) );

	//Centred on 1 so that the tooth redistributes ink rather than removing it:
	//the mean deposit over any patch of sheet is what it would have been on a
	//perfectly flat one.
	return max( 0.0, 1.0 + amount * ( 2.0 * n - 1.0 ) );
}
)";

// ---------------------------------------------------------------------------
// The full-screen quad.
// ---------------------------------------------------------------------------
//
// uv stays 0..1 and is NOT scaled by the host texture's MaxUV here. Exactly one
// pass reads a host texture, and in that pass the same uv is also the geometry
// of the sheet; scaling it by the host's padding would shrink the paper
// whenever Resolume handed over a texture larger than its picture -- which
// happens on some machines and not others.
const char* const kScreenVertexBody = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

// ---------------------------------------------------------------------------
// The ink: one instanced quad per step interval.
// ---------------------------------------------------------------------------
//
// The two attributes are the same buffer bound twice, the second offset by one
// element, so instance i sees step i and step i+1 with nothing duplicated and
// nothing uploaded twice. The instance count must therefore be count-1: drawing
// count instances reads one Step past the end, which on most drivers returns
// zeroes and on some returns whatever was there -- a stroke from the end of the
// figure to somewhere arbitrary, on some machines and not others.
//
// `stepA` / `stepB` because `sample` is reserved.
const char* const kInkVertexBody = R"(
layout( location = 0 ) in vec4 stepA;//x, y paper; z pen pressure; w dt seconds
layout( location = 1 ) in vec4 stepB;

uniform float Flow;        //ink per quantum -- see PerDistance
uniform float PerDistance; //1 for a ballpoint (per unit length), 0 for a fibre tip (per second)
uniform float NibSigma;    //paper units; 1 unit = half the sheet's height
uniform float NibSpread;   //how much a pressed nib swells
uniform float DensityFloor;//peak areal density below which a segment is skipped
uniform float Scale;       //sheet units per paper unit: the operator's zoom
uniform vec2  Centre;      //where the ring's centre sits on the sheet
uniform float SheetAspect; //sheet width / sheet height

flat out float segLength;
flat out float segSigma;
flat out float segAmount;
flat out vec2  segClipUV;
out vec2 segUV;
out vec2 segPaper;

//---------------------------------------------------------------------------
// Not isnan()/isinf(). Those are the right functions and also the first thing a
// compiler with fast-math assumptions folds to constant false, on the grounds
// that the value cannot happen. The value is exactly what this is here about.
//---------------------------------------------------------------------------
bool usable( float v )
{
	return v > -1e30 && v < 1e30;
}

bool usable2( vec2 v )
{
	return usable( v.x ) && usable( v.y );
}

void main()
{
	vec2 a = stepA.xy * Scale + Centre;
	vec2 b = stepB.xy * Scale + Centre;

	//Pen pressure over the interval, averaged across its two ends. A lifted pen
	//is exactly zero and deposits nothing -- which is how a layer change draws
	//no line across the sheet.
	float press = clamp( 0.5 * ( stepA.z + stepB.z ), 0.0, 1.0 );

	vec2 delta = b - a;
	float span = length( delta );

	//The whole darkness model, in one line, and it is the same line for both
	//pens: a quantum of ink per step interval, which the fragment stage then
	//spreads over however far the nib went. Nothing anywhere divides by a
	//speed.
	//
	//What differs is what a quantum is worth. A ballpoint's ball turns as it
	//travels, so it is `length`, and the spreading below cancels it exactly --
	//a line the same darkness however fast the hand moved. A fibre tip feeds by
	//capillary action, so it is `dt`, nothing cancels, and the dwell law falls
	//out: slow parts of the figure bloom. `dt` and `length` are both per-step
	//precisely so that this is independent of how many steps the crank emitted.
	float quantum = PerDistance > 0.5 ? span : max( stepA.w, 0.0 );
	float amount  = Flow * quantum * press;

	//A pressed nib spreads. The sigma also scales with the operator's zoom, so
	//that a line is a line on the sheet rather than a fixed number of pixels
	//that gets fatter relative to the figure as the figure is made smaller.
	float sigma = NibSigma * Scale * ( 1.0 + NibSpread * press );

	vec2 dir = span > 1e-9 ? delta / span : vec2( 1.0, 0.0 );

	float len = max( span, 0.05 * sigma );

	//Peak areal density of the finished segment. Culling on this rather than on
	//amount is what makes the cull mean something: a fast stroke and a slow one
	//can carry identical ink and only one of them is visible.
	float density = amount / max( len * sigma * Sqrt2Pi, 1e-30 );

	bool ok = usable2( a ) && usable2( b )
	       && usable( amount ) && usable( sigma )
	       && amount > 0.0 && sigma > 0.0
	       && density >= DensityFloor;

	if( !ok )
	{
		//A degenerate quad: the same clip position for all four corners, so it
		//has no area whatever the driver does with it, and outside the frustum
		//as well. Returning without writing gl_Position is undefined, and
		//discarding in the fragment stage would be too late -- a NaN position
		//can already have taken the primitive somewhere enormous.
		segLength = 0.0;
		segSigma  = 1.0;
		segAmount = 0.0;
		segClipUV = vec2( 0.0 );
		segUV     = vec2( 0.0 );
		segPaper  = vec2( 0.0 );
		gl_Position = vec4( 2.0, 2.0, 0.0, 1.0 );
		return;
	}

	//An oriented box around the capsule, padded by Extent sigma on all sides.
	float halfAlong  = 0.5 * len + Extent * sigma;
	float halfAcross = Extent * sigma;

	//Corners from the vertex index as a triangle strip: 0 -> (-,-), 1 -> (+,-),
	//2 -> (-,+), 3 -> (+,+). No vertex buffer for four signs.
	float sx = ( ( gl_VertexID & 1 ) == 0 ) ? -1.0 : 1.0;
	float sy = ( ( gl_VertexID & 2 ) == 0 ) ? -1.0 : 1.0;

	vec2 centre = a + dir * ( 0.5 * len );
	vec2 perp   = vec2( -dir.y, dir.x );
	vec2 pos    = centre + dir * ( sx * halfAlong ) + perp * ( sy * halfAcross );

	segLength = len;
	segSigma  = sigma;
	segAmount = amount;
	segUV     = vec2( sx * halfAlong, sy * halfAcross );
	segPaper  = pos;

	//Where the nib is, in the clip's coordinates, for a pen that takes its
	//colour off the footage. The MIDPOINT of the interval and not the fragment
	//position: the pen picks up what it is standing on, not what is under the
	//far edge of the blot it is about to make.
	segClipUV = vec2( 0.5 * ( centre.x / SheetAspect ) + 0.5, 0.5 * centre.y + 0.5 );

	//Paper units are isotropic -- one unit is half the sheet's height on both
	//axes -- so a circle drawn on it is round and stays round. The divide is the
	//only place the sheet's shape enters the ink pass at all.
	gl_Position = vec4( pos.x / SheetAspect, pos.y, 0.0, 1.0 );
}
)";

// ---------------------------------------------------------------------------
// The closed form, evaluated per fragment.
// ---------------------------------------------------------------------------
const char* const kInkFragmentBody = R"(
flat in float segLength;
flat in float segSigma;
flat in float segAmount;
flat in vec2  segClipUV;
in vec2 segUV;
in vec2 segPaper;

uniform vec3      InkAbsorb;   //-log( pen colour ), per channel
uniform float     InkFromClip; //1 when the pen takes its colour off the footage
uniform sampler2D ClipTexture;
uniform vec2      MaxUV;
uniform float     Tooth;
uniform float     ToothScale;

out vec4 fragColor;

void main()
{
	float inv = 1.0 / segSigma;
	float u   = segUV.x;
	float v   = segUV.y;

	//Across the segment: a normalised Gaussian, minus the value it has at the
	//quad's own boundary.
	//
	//Without the subtraction the profile is cut off at 4.5 sigma with a step of
	//about 1.5e-5 of the peak. For one stroke that is invisible. Where a
	//thousand strokes of a dense rosette overlap it is a thousand times 1.5e-5
	//along the line where those thousand boxes end -- a visible polygon edge
	//through the middle of the densest part of the drawing.
	float across   = InvSqrt2Pi * inv * exp( -0.5 * v * v * inv * inv );
	float pedestal = InvSqrt2Pi * inv * exp( -0.5 * Extent * Extent );
	across = max( across - pedestal, 0.0 );

	float along;
	if( segLength < 0.25 * segSigma )
	{
		//The point limit, taken explicitly rather than left to the CDF. A
		//difference of two nearly equal numbers, each carrying the tanh
		//approximation's ~3e-4 of error, has several percent of its own by the
		//time L is a quarter of a sigma -- and a short segment is precisely
		//what a pen at a cusp produces, which is the part of the figure the
		//plugin is about. `segLength` is flat, so the branch is dynamically
		//uniform across the primitive.
		along = InvSqrt2Pi * inv * exp( -0.5 * u * u * inv * inv );
	}
	else
	{
		float halfLen = 0.5 * segLength;//`half` is reserved
		along = ( ncdf( ( u + halfLen ) * inv ) - ncdf( ( u - halfLen ) * inv ) ) / segLength;
	}

	float deposit = segAmount * across * along * paperTooth( segPaper, Tooth, ToothScale );

	//Which pen. `InkAbsorb` is already -log( colour ): the buffer accumulates
	//absorption, never a colour, so this multiply is the only place a pen's
	//identity enters the sheet.
	vec3 absorb = InkAbsorb;
	if( InkFromClip > 0.5 )
	{
		vec2 clipUV = clamp( segClipUV, vec2( 0.0 ), vec2( 1.0 ) ) * MaxUV;
		vec3 picked = texture( ClipTexture, clipUV ).rgb;
		//Floored for the same reason Pens.cpp floors its own: exp(-9.2) is a
		//thousandth of the darkest step an 8-bit display has, and a pen blacker
		//than that is not blacker, it is a bigger number in a shared sum.
		absorb = -log( clamp( picked, vec3( 1.0e-4 ), vec3( 1.0 ) ) );
	}

	fragColor = vec4( absorb * deposit, 0.0 );
}
)";

// ---------------------------------------------------------------------------
// The sheet lightening.
// ---------------------------------------------------------------------------
//
// Drawn with glBlendFunc( GL_ZERO, GL_SRC_COLOR ), which computes dst = src *
// dst -- a multiply in place. That is what lets the paper be ONE buffer with no
// ping-pong: the alternative reads a history texture and writes a second
// buffer, which for a full-resolution RGBA32F sheet is a second allocation and
// a full-frame copy every frame, for a pass whose entire job is one multiply.
const char* const kFadeFragmentBody = R"(
in vec2 uv;
uniform float Retain;//exp( -dt / tau ), already computed on the CPU

out vec4 fragColor;

void main()
{
	//Alpha is 1 so that the blend leaves it alone. Nothing reads it; it is
	//there because an RGBA target has it.
	fragColor = vec4( vec3( Retain ), 1.0 );
}
)";

// ---------------------------------------------------------------------------
// The sheet.
// ---------------------------------------------------------------------------
const char* const kSheetFragmentBody = R"(
in vec2 uv;

uniform sampler2D PaperTexture;//accumulated optical density
uniform sampler2D ClipTexture;
uniform vec2  MaxUV;
uniform float SheetAspect;

uniform vec3  PaperColour;
uniform float PaperFromClip; //1 when the footage is the sheet the pen draws on
uniform float PaperGrain;    //visible tooth, distinct from the deposit tooth
uniform float ToothScale;
uniform float Negative;      //show the drawing photographed and printed inverted

uniform float Opacity;
uniform float Passthrough;   //1 when the plugin should get out of the way

// The gears, drawn as an overlay rather than as geometry.
uniform float GearLevel;
uniform float Scale;
uniform vec2  Centre;
uniform float RingRadius;
uniform vec2  WheelCentre;
uniform float WheelRadius;
uniform vec2  PenPoint;
uniform float RingTeeth;
uniform float WheelTeeth;
uniform float WheelAngle;
uniform vec3  GearColour;

out vec4 fragColor;

//---------------------------------------------------------------------------
// A toothed circle, as a signed distance.
//
// The teeth are a radial ripple of the circle's own radius at the gear's tooth
// count, which is not a gear tooth profile and is not trying to be: at the size
// this is ever drawn, an involute flank and a sinusoid are the same handful of
// pixels. What matters is that the COUNT is right, because the whole plugin is
// about the count, and an operator looking at the overlay to see whether 96 and
// 32 mesh should be able to count them.
//---------------------------------------------------------------------------
float toothedRing( vec2 p, vec2 centre, float radius, float teeth, float phase, float depth )
{
	vec2 d = p - centre;
	float r = length( d );
	float a = atan( d.y, d.x ) - phase;
	float ripple = depth * radius * sin( teeth * a );
	return abs( r - ( radius + ripple ) );
}

float lineMask( float distance, float width )
{
	float rate = max( fwidth( distance ), 1e-6 );
	return 1.0 - smoothstep( width - rate, width + rate, distance );
}

void main()
{
	vec2 clipUV = uv * MaxUV;
	vec3 clip   = texture( ClipTexture, clipUV ).rgb;
	float clipAlpha = texture( ClipTexture, clipUV ).a;

	//Paper coordinates: 1 unit is half the sheet's height, x widened by the
	//aspect. The same convention the ink pass uses, and the reason a figure is
	//round on a 16:9 output.
	vec2 paper = vec2( ( uv.x - 0.5 ) * 2.0 * SheetAspect, ( uv.y - 0.5 ) * 2.0 );

	vec3 density = max( texture( PaperTexture, uv ).rgb, vec3( 0.0 ) );

	vec3 sheet = mix( PaperColour, clip, PaperFromClip );

	//The tooth, visible this time. A separate control from the one in the ink
	//pass: that one decides how unevenly the sheet TAKES ink, this one decides
	//how much of the sheet you can see. A drawing on smooth board takes ink
	//evenly and still shows no grain; cartridge does both.
	if( PaperGrain > 0.0 )
		sheet *= paperTooth( paper, PaperGrain * 0.5, ToothScale );

	//Beer's law. The one line the whole renderer exists to be able to write.
	vec3 drawn = sheet * exp( -density );

	//The print. Ink only subtracts, so a pale line on a dark ground is not
	//something the machine can make -- but a negative of a drawing that could
	//be made is, and that is what this is. Applied before the gear overlay, so
	//the overlay keeps the colour it was given rather than being inverted with
	//everything else.
	drawn = mix( drawn, vec3( 1.0 ) - drawn, clamp( Negative, 0.0, 1.0 ) );

	//---------------------------------------------------------------------
	// The gears.
	//---------------------------------------------------------------------
	if( GearLevel > 0.0 )
	{
		//Back into the machine's own coordinates, where the ring's pitch radius
		//is exactly 1. Doing the overlay here rather than in paper units is
		//what lets every number below be a fraction of the ring.
		vec2 p = ( paper - Centre ) / max( Scale, 1e-6 );

		//A tooth is about half a pitch deep, and a pitch is 2*pi*r/teeth, so
		//the ripple's amplitude as a FRACTION of the radius is about pi/teeth.
		//Written that way it is right for every gear in the range rather than
		//tuned for one: a 24-tooth wheel gets chunky teeth and a 150-tooth ring
		//gets fine ones, which is what the plastic does.
		float ringRipple  = 1.4 / max( RingTeeth, 1.0 );
		float wheelRipple = 1.4 / max( WheelTeeth, 1.0 );

		//A line a little over a pixel wide at 1080p, expressed as a fraction of
		//the ring so that it is the same line at every raster and every Size.
		float width = 0.004 / max( Scale, 1e-6 );

		float ring  = lineMask( toothedRing( p, vec2( 0.0 ), RingRadius, RingTeeth, 0.0, ringRipple ), width );
		float wheel = lineMask( toothedRing( p, WheelCentre, WheelRadius, WheelTeeth, WheelAngle, wheelRipple ), width );

		//The pen, and the arm from the wheel's centre out to it: the two things
		//that say WHY the line is where it is. Without the arm the wheel is a
		//circle rolling around and the drawing is unexplained.
		float pen = 1.0 - smoothstep( 2.0 * width, 3.0 * width, length( p - PenPoint ) );

		vec2 arm     = PenPoint - WheelCentre;
		float armLen = max( length( arm ), 1e-6 );
		vec2 armDir  = arm / armLen;
		vec2 rel     = p - WheelCentre;
		float t      = clamp( dot( rel, armDir ), 0.0, armLen );
		float armMask = lineMask( length( rel - armDir * t ), width * 0.6 );

		float gear = max( max( ring, wheel ), max( pen, armMask ) ) * GearLevel;
		drawn = mix( drawn, GearColour, gear );
	}

	//---------------------------------------------------------------------
	// Out.
	//---------------------------------------------------------------------
	//
	// Passthrough is exact by construction: at 1 the clip leaves untouched,
	// including its alpha, whatever every other control says. That is what
	// makes Mix at zero a guarantee rather than an approximation.
	vec3 outColour = mix( drawn, clip, Passthrough );
	float outAlpha = mix( 1.0, clipAlpha, Passthrough );

	fragColor = vec4( outColour, outAlpha ) * Opacity;
}
)";

} // namespace

std::string vertexSource( const char* body )
{
	std::string source = kConstants;
	source += body;
	return source;
}

std::string fragmentSource( const char* body )
{
	std::string source = kConstants;
	source += kFragmentHelpers;
	source += body;
	return source;
}

const std::string& screenVertex()
{
	static const std::string source = vertexSource( kScreenVertexBody );
	return source;
}

const std::string& inkVertex()
{
	static const std::string source = vertexSource( kInkVertexBody );
	return source;
}

const std::string& inkFragment()
{
	static const std::string source = fragmentSource( kInkFragmentBody );
	return source;
}

const std::string& fadeFragment()
{
	static const std::string source = fragmentSource( kFadeFragmentBody );
	return source;
}

const std::string& sheetFragment()
{
	static const std::string source = fragmentSource( kSheetFragmentBody );
	return source;
}

} // namespace cogwheel::shaders
