/// The OpenFX builds of Cogwheel, for DaVinci Resolve, Nuke, Natron, Vegas and
/// other OFX hosts. Two plugins from this one file, as the FFGL side ships two
/// bundles: "Cogwheel" is a generator and "Cogwheel Ink" draws over the
/// incoming clip.
///
/// ===========================================================================
/// What is shared, and what is mirrored
/// ===========================================================================
///
/// **The whole machine is shared.** `Gears`, `Wheels`, `Crank`, `Pens` and
/// `Controls.cpp`'s `Resolve` touch no GL at all, which is exactly why they can
/// be linked straight in here: the gear arithmetic, the slip, the layer
/// sequence, the palette and every slider's mapping are literally the same
/// objects `cgtest` measures. `Presets.h` is the same table, bound in the same
/// order and asserted against `presets::kParamCount` below.
///
/// What is mirrored is the **renderer**, because the renderer is GLSL. The
/// `cpu` namespace below is a transcription of `render/Shaders.cpp`, constant
/// for constant: the ink pass's closed form and its pedestal subtraction, the
/// paper's tooth, the fade, Beer's law, the negative and the gear overlay.
/// **When editing one of those, edit the matching function here.**
///
/// ===========================================================================
/// Why the CPU, and not the OFX OpenGL render suite
/// ===========================================================================
///
/// An OFX host hands a plugin a buffer and expects pixels back.
/// `kOfxOpenGLRender` exists, it is optional, and no host guarantees it:
/// Resolve will call the CPU path whenever it feels like it, and Natron has no
/// GL path at all. A plugin that only knows how to draw through GL is a plugin
/// that does not render. So this build asks for no context and creates none,
/// the same choice every other OpenFX port in this fleet made.
///
/// It is cheaper here than in most of them. The ink pass touches only the box
/// around each step -- about a dozen pixels square for a fine nib -- and a
/// frame is a hundred or so steps, so **depositing a frame is a few tens of
/// thousands of pixel evaluations** rather than a full-frame pass. The sheet
/// pass is full-frame and threaded. What is expensive is the replay, below.
///
/// ===========================================================================
/// Frame order, and the one thing that genuinely differs
/// ===========================================================================
///
/// **A drawing is history.** That is not an implementation detail to be worked
/// around -- it is the plugin. The sheet at frame 900 is every stroke laid down
/// since the drawing started, so unlike most of this fleet Cogwheel is not a
/// pure function of (time, parameters) and cannot be made into one.
///
/// So this build **replays from the beginning**. The instance remembers the
/// frame it last simulated and steps forward one frame at a time, which makes a
/// linear render exact and costs nothing per frame beyond that frame's own
/// strokes. Asking for an earlier frame restarts from frame zero and runs
/// forward into the requested one.
///
/// The honest cost, stated plainly: **scrubbing backwards through a long
/// drawing is slow**, and it gets slower the further in you are. There is no
/// warm-up window that would fix it, because a warm-up window would produce a
/// *different drawing* -- one missing everything laid down before the window
/// opened. Vectrix can bound its replay at two seconds because a phosphor
/// forgets; paper does not. Render linearly and this never arises.
///
/// ===========================================================================
/// Inert or different on OFX by design
/// ===========================================================================
///
/// The controls below are declared, keep their positions and, where noted, do
/// nothing. They are present rather than absent because the preset table stores
/// dropdown element *values*, and a build with a shorter list is a build where
/// a preset selects the wrong entry.
///
/// - **No tempo.** OFX carries no transport tempo, so `Resolve` is handed a
///   fixed 120 bpm. The Sync dropdown therefore works, and works off 120 rather
///   than off the edit. Crank is the control that behaves identically in both
///   builds.
/// - **Ink from Clip is exact on a linear render and approximate on a seek.**
///   The pen picks up the colour of the clip it is passing over, which needs
///   the clip's image at the frame the stroke was laid down on. During a
///   forward render that is the frame being rendered and the result is right.
///   During a catch-up replay after a backward seek, every replayed frame is
///   given the requested frame's image, because reaching another frame's pixels
///   would mean declaring temporal clip access and fetching hundreds of images
///   the plugin looks at once each.
/// - **New Sheet is a restart.** In FFGL it wipes the paper of a free-running
///   drawing; here the drawing at a frame is decided by the frame, so pressing
///   it throws the replay away and starts it again -- which produces the same
///   picture. It is kept so that the two parameter lists match.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Controls.h"
#include "../Presets.h"
#include "../machine/Crank.h"
#include "../machine/Gears.h"
#include "../machine/Pens.h"
#include "../machine/Step.h"

namespace
{
using namespace cogwheel;

constexpr const char* kSourceIdentifier = "com.stoatworks.cogwheel";
constexpr const char* kInkIdentifier    = "com.stoatworks.cogwheelink";
constexpr const char* kPluginGrouping   = "Stoatworks";

constexpr const char* kPluginDescription =
	"A Spirograph: a toothed ring, a wheel rolling in mesh with it, and a pen "
	"in one of the wheel's holes.\n\n"
	"Nothing here evaluates a curve. Set the two tooth counts and the figure "
	"follows -- 96 and 32 mesh three to one, so that one closes in a single "
	"turn with three lobes, while 96 and 31 take thirty-one turns and have "
	"ninety-six.\n\n"
	"Ink is subtractive: the sheet accumulates optical density and what you see "
	"is the paper through it, so a second pen crossing the first darkens it the "
	"way it does on paper rather than brightening it the way an additive "
	"renderer would. Layers draws a stack of figures, lifting the pen and "
	"changing hole and colour each time one closes.\n\n"
	"A closed figure retraces its own line for ever. Creep is a mesh very "
	"slightly out of true, and it is what keeps a drawing growing -- as it does "
	"on the real thing.\n\n"
	"This OpenFX build renders on the CPU: there is no OpenGL context to be had "
	"in an OFX host. A drawing is history, so it replays from the start of the "
	"clip; render forwards.\n\n"
	"https://stoatworks-labs.com";

/// Mirrors `render/Shaders.cpp`'s kConstants. `Extent` is the half-width of the
/// ink pass's box in units of sigma, and the fragment stage subtracts the
/// profile's value at exactly this distance, so the two have to agree.
constexpr float kExtent     = 4.5f;
constexpr float kSqrt2Pi    = 2.50662827463100050f;
constexpr float kInvSqrt2Pi = 0.39894228040143268f;

inline float saturate( float v )
{
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

/// GLSL smoothstep.
inline float smoothstep( float edge0, float edge1, float x )
{
	const float t = saturate( ( x - edge0 ) / ( edge1 - edge0 ) );
	return t * t * ( 3.0f - 2.0f * t );
}

/// GLSL mix.
inline float mix( float a, float b, float t )
{
	return a + ( b - a ) * t;
}

//===========================================================================
// The shared fragment helpers, transcribed.
//===========================================================================

/// `ncdf` from the prelude. The tanh approximation, and the same clamp -- which
/// is not tidiness there and is not here: a tanh computed as
/// (e^2x - 1)/(e^2x + 1) overflows around x = 44 and produces inf/inf.
inline float ncdf( float x )
{
	const float t = std::clamp( 0.7978845608f * ( x + 0.044715f * x * x * x ), -8.0f, 8.0f );
	return 0.5f * ( 1.0f + std::tanh( t ) );
}

inline float fract( float x )
{
	return x - std::floor( x );
}

/// The sin-fract hash from the prelude.
///
/// **This will not be bit-identical to the GPU's.** `sin` of a large argument
/// is where implementations differ most, and the hash multiplies by 43758 and
/// takes the fraction -- so a difference in the last bit of the sine is a
/// completely different value. The GRAIN therefore differs in detail between
/// the two builds while having the same statistics, which is what it is for.
/// Nothing else in either renderer depends on it.
inline float hash21( float px, float py )
{
	return fract( std::sin( px * 127.1f + py * 311.7f ) * 43758.5453123f );
}

inline float valueNoise( float px, float py )
{
	const float ix = std::floor( px );
	const float iy = std::floor( py );
	const float fx = px - ix;
	const float fy = py - iy;
	const float ux = fx * fx * ( 3.0f - 2.0f * fx );
	const float uy = fy * fy * ( 3.0f - 2.0f * fy );

	const float a = hash21( ix, iy );
	const float b = hash21( ix + 1.0f, iy );
	const float c = hash21( ix, iy + 1.0f );
	const float d = hash21( ix + 1.0f, iy + 1.0f );

	return mix( mix( a, b, ux ), mix( c, d, ux ), uy );
}

inline float paperTooth( float px, float py, float amount, float scale )
{
	if( amount <= 0.0f )
		return 1.0f;

	const float n = 0.65f * valueNoise( px * scale, py * scale )
	              + 0.35f * valueNoise( px * scale * 2.7f + 17.0f, py * scale * 2.7f + 5.0f );

	//Centred on 1, so the tooth redistributes ink rather than removing it.
	return std::max( 0.0f, 1.0f + amount * ( 2.0f * n - 1.0f ) );
}

//===========================================================================
// The sheet, on the CPU.
//
// Three floats a pixel, bottom row first -- exactly as GL stores a texture and
// exactly as OFX hands its images over, so nothing anywhere in this file flips
// a row. It holds optical DENSITY and never a colour; see render/Shaders.h.
//===========================================================================
class Paper
{
public:
	void ensure( int width, int height )
	{
		if( w == width && h == height )
			return;
		w = width;
		h = height;
		density.assign( static_cast< size_t >( w ) * h * 3, 0.0f );
	}

	void clear()
	{
		std::fill( density.begin(), density.end(), 0.0f );
	}

	/// The fade pass: a multiply in place, exactly as the GL build's
	/// glBlendFunc( GL_ZERO, GL_SRC_COLOR ) does it.
	void fade( float retain )
	{
		if( retain >= 1.0f )
			return;
		for( float& v : density )
			v *= retain;
	}

	int width() const { return w; }
	int height() const { return h; }

	const float* at( int x, int y ) const
	{
		return density.data() + ( static_cast< size_t >( y ) * w + x ) * 3;
	}

	float* at( int x, int y )
	{
		return density.data() + ( static_cast< size_t >( y ) * w + x ) * 3;
	}

	bool ready() const { return w > 0 && h > 0 && !density.empty(); }

private:
	int w = 0, h = 0;
	std::vector< float > density;
};

/// Paper coordinates of a pixel centre. The ink pass's `gl_Position` divides x
/// by the aspect and leaves y alone, so screen u,v map back to exactly this --
/// and it is the same expression the sheet pass uses for its own `paper`.
inline void paperAt( int x, int y, int w, int h, float aspect, float& px, float& py )
{
	px = ( ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( w ) - 0.5f ) * 2.0f * aspect;
	py = ( ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( h ) - 0.5f ) * 2.0f;
}

/// What the deposit needs, which is the RenderParams the GL build hands its ink
/// shader plus the ink colour of the run being drawn.
struct DepositParams
{
	float flow      = 1.0f;
	bool perDistance = true;
	float nibSigma  = 0.004f;
	float nibSpread = 0.15f;
	float densityFloor = 1.0e-5f;
	float scale     = 1.0f;
	float centre[ 2 ] = { 0.0f, 0.0f };
	float tooth     = 0.0f;
	float toothScale = 220.0f;
	float aspect    = 16.0f / 9.0f;
};

/// One step interval, transcribed from `kInkVertexBody` and
/// `kInkFragmentBody`.
///
/// The vertex stage's oriented box becomes a bounding rectangle in pixels and
/// the fragment stage's closed form is evaluated at each pixel centre inside
/// it. Everything culled there is culled here, in the same order and against
/// the same numbers -- including the pedestal subtraction, which matters for
/// exactly the same reason: a dense figure has thousands of these boxes ending
/// on the same lines.
void depositSegment( Paper& paper, const Step& a, const Step& b,
                     const float absorb[ 3 ], const DepositParams& p )
{
	const float ax = a.x * p.scale + p.centre[ 0 ];
	const float ay = a.y * p.scale + p.centre[ 1 ];
	const float bx = b.x * p.scale + p.centre[ 0 ];
	const float by = b.y * p.scale + p.centre[ 1 ];

	const float press = std::clamp( 0.5f * ( a.ink + b.ink ), 0.0f, 1.0f );

	const float dx   = bx - ax;
	const float dy   = by - ay;
	const float span = std::sqrt( dx * dx + dy * dy );

	const float quantum = p.perDistance ? span : std::max( a.dt, 0.0f );
	const float amount  = p.flow * quantum * press;

	const float sigma = p.nibSigma * p.scale * ( 1.0f + p.nibSpread * press );

	const float dirX = span > 1e-9f ? dx / span : 1.0f;
	const float dirY = span > 1e-9f ? dy / span : 0.0f;

	const float len = std::max( span, 0.05f * sigma );

	const float areal = amount / std::max( len * sigma * kSqrt2Pi, 1e-30f );

	const bool usable = std::isfinite( ax ) && std::isfinite( ay )
	                 && std::isfinite( bx ) && std::isfinite( by )
	                 && std::isfinite( amount ) && std::isfinite( sigma )
	                 && amount > 0.0f && sigma > 0.0f
	                 && areal >= p.densityFloor;
	if( !usable )
		return;

	const float halfAlong  = 0.5f * len + kExtent * sigma;
	const float halfAcross = kExtent * sigma;

	const float cx = ax + dirX * ( 0.5f * len );
	const float cy = ay + dirY * ( 0.5f * len );

	//The bounding rectangle of the oriented box, in paper units, then in
	//pixels. `reach` is the box's half-diagonal projected onto each axis, which
	//is the tightest axis-aligned bound that cannot clip a corner off.
	const float reachX = std::fabs( dirX ) * halfAlong + std::fabs( dirY ) * halfAcross;
	const float reachY = std::fabs( dirY ) * halfAlong + std::fabs( dirX ) * halfAcross;

	const int w = paper.width();
	const int h = paper.height();

	const float pxPerUnitY = 0.5f * static_cast< float >( h );
	const float pxPerUnitX = 0.5f * static_cast< float >( w ) / p.aspect;

	const int x0 = std::max( 0, static_cast< int >( std::floor( ( ( cx - reachX ) / ( 2.0f * p.aspect ) + 0.5f ) * static_cast< float >( w ) - 0.5f ) ) );
	const int x1 = std::min( w - 1, static_cast< int >( std::ceil( ( ( cx + reachX ) / ( 2.0f * p.aspect ) + 0.5f ) * static_cast< float >( w ) + 0.5f ) ) );
	const int y0 = std::max( 0, static_cast< int >( std::floor( ( ( cy - reachY ) * 0.5f + 0.5f ) * static_cast< float >( h ) - 0.5f ) ) );
	const int y1 = std::min( h - 1, static_cast< int >( std::ceil( ( ( cy + reachY ) * 0.5f + 0.5f ) * static_cast< float >( h ) + 0.5f ) ) );

	( void )pxPerUnitX;
	( void )pxPerUnitY;

	if( x1 < x0 || y1 < y0 )
		return;

	const float inv      = 1.0f / sigma;
	const float pedestal = kInvSqrt2Pi * inv * std::exp( -0.5f * kExtent * kExtent );
	const bool shortSeg  = len < 0.25f * sigma;
	const float halfLen  = 0.5f * len;

	for( int y = y0; y <= y1; ++y )
	{
		for( int x = x0; x <= x1; ++x )
		{
			float px, py;
			paperAt( x, y, w, h, p.aspect, px, py );

			const float relX = px - cx;
			const float relY = py - cy;

			const float u = relX * dirX + relY * dirY;
			const float v = -relX * dirY + relY * dirX;

			if( std::fabs( u ) > halfAlong || std::fabs( v ) > halfAcross )
				continue;

			float across = kInvSqrt2Pi * inv * std::exp( -0.5f * v * v * inv * inv );
			across       = std::max( across - pedestal, 0.0f );
			if( across <= 0.0f )
				continue;

			const float along = shortSeg
				? kInvSqrt2Pi * inv * std::exp( -0.5f * u * u * inv * inv )
				: ( ncdf( ( u + halfLen ) * inv ) - ncdf( ( u - halfLen ) * inv ) ) / len;

			const float deposit = amount * across * along * paperTooth( px, py, p.tooth, p.toothScale );
			if( !( deposit > 0.0f ) )
				continue;

			float* cell = paper.at( x, y );
			cell[ 0 ] += absorb[ 0 ] * deposit;
			cell[ 1 ] += absorb[ 1 ] * deposit;
			cell[ 2 ] += absorb[ 2 ] * deposit;
		}
	}
}

//===========================================================================
// The sheet pass, transcribed from kSheetFragmentBody.
//===========================================================================
struct SheetSetup
{
	const Paper* paper = nullptr;

	float aspect = 16.0f / 9.0f;

	float paperColour[ 3 ] = { 0.94f, 0.92f, 0.86f };
	bool paperFromClip     = false;
	float paperGrain       = 0.15f;
	float toothScale       = 220.0f;
	bool negative          = false;

	float opacity     = 1.0f;
	float passthrough = 0.0f;

	float gearLevel = 0.0f;
	float gearColour[ 3 ] = { 0.36f, 0.40f, 0.46f };
	float scale     = 1.0f;
	float centre[ 2 ] = { 0.0f, 0.0f };
	float wheelCentre[ 2 ] = { 0.0f, 0.0f };
	float wheelRadius = 0.5f;
	float penPoint[ 2 ] = { 0.0f, 0.0f };
	float ringTeeth  = 96.0f;
	float wheelTeeth = 52.0f;
	float wheelAngle = 0.0f;

	bool hasClip = false;
};

inline float toothedRing( float px, float py, float cx, float cy,
                          float radius, float teeth, float phase, float depth )
{
	const float dx = px - cx;
	const float dy = py - cy;
	const float r  = std::sqrt( dx * dx + dy * dy );
	const float a  = std::atan2( dy, dx ) - phase;
	const float ripple = depth * radius * std::sin( teeth * a );
	return std::fabs( r - ( radius + ripple ) );
}

/// The overlay's line mask. GLSL's `fwidth` is a finite difference over the 2x2
/// quad, so passing the real local rate in is not an approximation of the
/// shader -- it is the same thing the shader does.
inline float lineMask( float distance, float width, float rate )
{
	return 1.0f - smoothstep( width - rate, width + rate, distance );
}

//===========================================================================
// The processor.
//===========================================================================
class CogwheelProcessorBase : public OFX::ImageProcessor
{
public:
	explicit CogwheelProcessorBase( OFX::ImageEffect& effect ) : OFX::ImageProcessor( effect ) {}

	void setSetup( const SheetSetup* s, OFX::Image* src )
	{
		setup  = s;
		srcImg = src;
	}

protected:
	const SheetSetup* setup = nullptr;
	OFX::Image* srcImg      = nullptr;
};

template< class PIX, int nComponents, int maxValue >
class CogwheelProcessor : public CogwheelProcessorBase
{
public:
	explicit CogwheelProcessor( OFX::ImageEffect& effect ) : CogwheelProcessorBase( effect ) {}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const SheetSetup& s   = *setup;
		const OfxRectI bounds = _dstImg->getBounds();
		const int outW        = std::max( 1, bounds.x2 - bounds.x1 );
		const int outH        = std::max( 1, bounds.y2 - bounds.y1 );

		const float invW = 1.0f / static_cast< float >( outW );
		const float invH = 1.0f / static_cast< float >( outH );

		//The local rate the overlay's anti-aliasing needs: one pixel, expressed
		//in the machine's own coordinates.
		const float rate = std::max( 2.0f * invH / std::max( s.scale, 1e-6f ), 1e-6f );

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dstPix = static_cast< PIX* >( _dstImg->getPixelAddress( window.x1, y ) );
			const int py = y - bounds.y1;

			for( int x = window.x1; x < window.x2; ++x, dstPix += nComponents )
			{
				const int px = x - bounds.x1;

				float paperX, paperY;
				paperAt( px, py, outW, outH, s.aspect, paperX, paperY );

				float clip[ 4 ] = { 0.0f, 0.0f, 0.0f, 1.0f };
				if( s.hasClip && srcImg != nullptr )
				{
					const PIX* srcPix = static_cast< const PIX* >( srcImg->getPixelAddress( x, y ) );
					if( srcPix != nullptr )
					{
						for( int c = 0; c < std::min( nComponents, 4 ); ++c )
							clip[ c ] = static_cast< float >( srcPix[ c ] ) / static_cast< float >( maxValue );
						if( nComponents < 4 )
							clip[ 3 ] = 1.0f;
					}
				}

				float sheet[ 3 ];
				for( int c = 0; c < 3; ++c )
					sheet[ c ] = s.paperFromClip ? clip[ c ] : s.paperColour[ c ];

				if( s.paperGrain > 0.0f )
				{
					const float tooth = paperTooth( paperX, paperY, s.paperGrain * 0.5f, s.toothScale );
					for( int c = 0; c < 3; ++c )
						sheet[ c ] *= tooth;
				}

				float drawn[ 3 ] = { 0.0f, 0.0f, 0.0f };
				if( s.paper != nullptr && s.paper->ready()
				    && px < s.paper->width() && py < s.paper->height() )
				{
					const float* density = s.paper->at( px, py );
					for( int c = 0; c < 3; ++c )
						drawn[ c ] = sheet[ c ] * std::exp( -std::max( density[ c ], 0.0f ) );
				}
				else
				{
					for( int c = 0; c < 3; ++c )
						drawn[ c ] = sheet[ c ];
				}

				if( s.negative )
					for( int c = 0; c < 3; ++c )
						drawn[ c ] = 1.0f - drawn[ c ];

				if( s.gearLevel > 0.0f )
				{
					const float gx = ( paperX - s.centre[ 0 ] ) / std::max( s.scale, 1e-6f );
					const float gy = ( paperY - s.centre[ 1 ] ) / std::max( s.scale, 1e-6f );

					const float ringRipple  = 1.4f / std::max( s.ringTeeth, 1.0f );
					const float wheelRipple = 1.4f / std::max( s.wheelTeeth, 1.0f );
					const float width       = 0.004f / std::max( s.scale, 1e-6f );

					const float ring = lineMask(
						toothedRing( gx, gy, 0.0f, 0.0f, 1.0f, s.ringTeeth, 0.0f, ringRipple ), width, rate );
					const float wheel = lineMask(
						toothedRing( gx, gy, s.wheelCentre[ 0 ], s.wheelCentre[ 1 ],
						             s.wheelRadius, s.wheelTeeth, s.wheelAngle, wheelRipple ), width, rate );

					const float penDx = gx - s.penPoint[ 0 ];
					const float penDy = gy - s.penPoint[ 1 ];
					const float pen   = 1.0f - smoothstep( 2.0f * width, 3.0f * width,
					                                       std::sqrt( penDx * penDx + penDy * penDy ) );

					const float armX = s.penPoint[ 0 ] - s.wheelCentre[ 0 ];
					const float armY = s.penPoint[ 1 ] - s.wheelCentre[ 1 ];
					const float armLen = std::max( std::sqrt( armX * armX + armY * armY ), 1e-6f );
					const float adX = armX / armLen;
					const float adY = armY / armLen;
					const float relX = gx - s.wheelCentre[ 0 ];
					const float relY = gy - s.wheelCentre[ 1 ];
					const float t    = std::clamp( relX * adX + relY * adY, 0.0f, armLen );
					const float offX = relX - adX * t;
					const float offY = relY - adY * t;
					const float arm  = lineMask( std::sqrt( offX * offX + offY * offY ), width * 0.6f, rate );

					const float gear = std::max( std::max( ring, wheel ), std::max( pen, arm ) ) * s.gearLevel;
					for( int c = 0; c < 3; ++c )
						drawn[ c ] = mix( drawn[ c ], s.gearColour[ c ], gear );
				}

				//Passthrough is exact by construction: at 1 the clip leaves
				//untouched, including its alpha, whatever every other control
				//says. That is what makes Mix at zero a guarantee.
				float out[ 4 ];
				for( int c = 0; c < 3; ++c )
					out[ c ] = mix( drawn[ c ], clip[ c ], s.passthrough );
				out[ 3 ] = mix( 1.0f, clip[ 3 ], s.passthrough );

				for( int c = 0; c < nComponents; ++c )
				{
					const float value = ( c < 4 ? out[ c ] : 1.0f ) * s.opacity;
					dstPix[ c ] = static_cast< PIX >(
						std::clamp( value, 0.0f, 1.0f ) * static_cast< float >( maxValue ) + ( maxValue == 1 ? 0.0f : 0.5f ) );
				}
			}
		}
	}
};

//===========================================================================
// The parameter surface.
//===========================================================================
enum class Kind
{
	Slider,
	Count,
	Toggle,
	Option,
	Colour, ///< An OFX RGB param, standing in for the FFGL build's three ids.
	Button,
	Absent  ///< A component of a Colour: it has no parameter of its own here.
};

struct Decl
{
	unsigned int id;
	Kind kind;
	const char* name;
	const char* label;
	const char* hint;
	float def;
	float lo;
	float hi;
	const char* const* options;
	int optionCount;
	const char* group;///< non-null starts a new group, mirroring SetParamGroup
};

#define SLIDER( id, name, label, def, hint ) \
	{ id, Kind::Slider, name, label, hint, def, 0.0f, 1.0f, nullptr, 0, nullptr }
#define SLIDERG( id, name, label, def, hint, group ) \
	{ id, Kind::Slider, name, label, hint, def, 0.0f, 1.0f, nullptr, 0, group }
#define TOGGLE( id, name, label, def, hint ) \
	{ id, Kind::Toggle, name, label, hint, def, 0.0f, 1.0f, nullptr, 0, nullptr }
#define COUNTP( id, name, label, def, lo, hi, hint ) \
	{ id, Kind::Count, name, label, hint, def, lo, hi, nullptr, 0, nullptr }
#define COUNTG( id, name, label, def, lo, hi, hint, group ) \
	{ id, Kind::Count, name, label, hint, def, lo, hi, nullptr, 0, group }
#define OPTION( id, name, label, def, table, n, hint ) \
	{ id, Kind::Option, name, label, hint, def, 0.0f, 1.0f, table, n, nullptr }
#define OPTIONG( id, name, label, def, table, n, hint, group ) \
	{ id, Kind::Option, name, label, hint, def, 0.0f, 1.0f, table, n, group }
#define COLOUR( id, name, label, hint ) \
	{ id, Kind::Colour, name, label, hint, 0.0f, 0.0f, 1.0f, nullptr, 0, nullptr }
#define COLOURG( id, name, label, hint, group ) \
	{ id, Kind::Colour, name, label, hint, 0.0f, 0.0f, 1.0f, nullptr, 0, group }
#define ABSENT( id ) \
	{ id, Kind::Absent, nullptr, nullptr, nullptr, 0.0f, 0.0f, 1.0f, nullptr, 0, nullptr }

const Decl kDecls[] = {
	//--- Gears -------------------------------------------------------------
	COUNTG( PT_RING, "ringTeeth", "Ring Teeth", 96.0f, 12.0f, 360.0f,
	        "How many teeth the fixed ring has. This and Wheel Teeth are the whole "
	        "machine: their greatest common divisor decides how many lobes the figure "
	        "has and how many turns it takes to close.",
	        "Gears" ),
	COUNTP( PT_WHEEL, "wheelTeeth", "Wheel Teeth", 52.0f, 3.0f, 240.0f,
	        "How many teeth the rolling wheel has. 96 and 32 mesh three to one and "
	        "close in a single turn; 96 and 31 share no factor and take thirty-one." ),
	OPTION( PT_MESH, "mesh", "Mesh", 0.0f, kMeshNames, kMeshCount,
	        "Whether the wheel rolls round the inside of the ring or the outside. "
	        "Inside gives the rosette, outside gives the petal shape." ),
	SLIDER( PT_PEN, "penHole", "Pen Hole", 0.770f,
	        "Which hole in the wheel the pen sits in, as a fraction of the wheel's "
	        "radius. Near the rim gives long thin lobes; near the axle gives a circle." ),
	TOGGLE( PT_SNAP_SET, "snapToSet", "Snap to Set", 1.0f,
	        "Restrict Ring and Wheel to the tooth counts a real Spirograph set carries. "
	        "The figures people recognise are the ones those particular integers make." ),
	TOGGLE( PT_SNAP_HOLES, "snapToHoles", "Snap to Holes", 1.0f,
	        "Restrict the pen to one of the twelve modelled hole positions rather than "
	        "sliding continuously." ),

	//--- Crank -------------------------------------------------------------
	OPTIONG( PT_SYNC, "sync", "Sync", 0.0f, kSyncNames, kSyncCount,
	         "Free runs at the Crank rate. The bar settings give the figure that many "
	         "bars to complete in, whatever this train's turn count is -- so two "
	         "different wheels finish together. OpenFX carries no transport tempo, so "
	         "this build works off a fixed 120 bpm.",
	         "Crank" ),
	SLIDER( PT_RATE, "crank", "Crank", 0.737f,
	        "Turns of the ring per second, 0.01 to 10, exponentially. Free mode only." ),
	OPTION( PT_DETAIL, "detail", "Detail", 1.0f, kDetailNames, kDetailCount,
	        "How finely the pen path is walked: 360, 1440 or 5760 steps a turn. A cost "
	        "dial with a visible symptom -- at Draft a big figure is faintly polygonal. "
	        "It does not change how much ink goes on the sheet." ),
	{ PT_RESET, Kind::Button, "newSheet", "New Sheet",
	  "Throw the drawing away and start again. In Resolume this wipes a free-running "
	  "sheet; here the drawing at a frame is decided by the frame, so it is a restart "
	  "of the replay and produces the same picture.",
	  0.0f, 0.0f, 1.0f, nullptr, 0, nullptr },
	COUNTP( PT_SEED, "seed", "Seed", 1.0f, 1.0f, 9999.0f,
	        "Which stack of layers, and where the gears skip." ),

	//--- Slip --------------------------------------------------------------
	SLIDERG( PT_CREEP, "creep", "Creep", 0.06f,
	         "Teeth per turn that the mesh is out of true. This is what keeps a drawing "
	         "growing: a closed figure retraces its own line for ever, and a mesh a "
	         "fraction of a tooth out precesses instead. Zero is a perfect gear, which "
	         "is a gear that stops drawing.",
	         "Slip" ),
	SLIDER( PT_SKIP, "skipChance", "Skip Chance", 0.0f,
	        "Probability per turn that the wheel jumps its mesh -- the classic way a "
	        "Spirograph drawing is ruined." ),
	COUNTP( PT_SKIP_TEETH, "skipSize", "Skip Size", 1.0f, 1.0f, 12.0f,
	        "How many teeth a jump goes." ),

	//--- Layers ------------------------------------------------------------
	COUNTG( PT_LAYERS, "layers", "Layers", 4.0f, 1.0f, static_cast< float >( kMaxLayers ),
	        "How many figures are drawn on one sheet. Each time one closes the pen is "
	        "lifted, moved and changed for the next colour -- which is how the "
	        "multicoloured Spirograph drawing everybody remembers is actually made. At "
	        "1 the pen never lifts and Creep is the only thing keeping it alive.",
	        "Layers" ),
	OPTION( PT_CHANGE, "onClosing", "On Closing", 0.0f, kChangeNames, kChangeCount,
	        "What moves when a figure closes and the next layer starts." ),
	TOGGLE( PT_WIPE, "wipeSheet", "Wipe Sheet", 1.0f,
	        "Start a fresh sheet when the whole stack is finished, rather than drawing "
	        "the next stack over it." ),

	//--- Pen ---------------------------------------------------------------
	OPTIONG( PT_PEN_SET, "pens", "Pens", 1.0f, kPenSetNames, kPenSetCount,
	         "Which pens the layers are drawn with. Four Pens is the set that came in "
	         "the box.",
	         "Pen" ),
	OPTION( PT_PEN_TYPE, "penType", "Pen Type", 0.0f, kPenTypeNames, kPenTypeCount,
	        "A ballpoint's ball rolls, so it lays down ink per unit of distance and a "
	        "line is the same darkness however fast the hand moved -- which is what "
	        "came in the box. A fibre tip feeds per unit of time, so it blooms wherever "
	        "the pen slows down, which at a cusp is a great deal." ),
	COLOUR( PT_INK_R, "ink", "Ink",
	        "The pen's colour, used when Pens is set to Ink Colour. It is an absorption, "
	        "not a light: a pen of this colour laid down once transmits exactly this "
	        "colour, and laid down twice transmits its square." ),
	ABSENT( PT_INK_G ),
	ABSENT( PT_INK_B ),
	SLIDER( PT_FLOW, "flow", "Flow", 0.625f,
	        "How dark the line is. Multiplied by the nib's width, so widening the nib "
	        "does not lighten the line -- a broader pen delivers proportionally more "
	        "ink, which is what makes it broader rather than blurrier." ),
	SLIDER( PT_NIB, "nib", "Nib", 0.398f,
	        "The nib's width, as a fraction of the sheet -- so it is the same line at "
	        "every output resolution." ),
	SLIDER( PT_SPREAD, "pressure", "Pressure", 0.10f,
	        "How much a pressed nib spreads." ),
	TOGGLE( PT_INK_FROM_CLIP, "inkFromClip", "Ink from Clip", 0.0f,
	        "The pen picks up the colour of the clip it is passing over, so the drawing "
	        "is built out of the footage rather than laid on top of it. Exact on a "
	        "linear render; after a backward seek the replayed frames use the current "
	        "frame's image. Effect build only." ),

	//--- Paper -------------------------------------------------------------
	COLOURG( PT_PAPER_R, "paper", "Paper",
	         "The sheet. Warm and slightly grey rather than white: pure white behind a "
	         "coloured line reads as a plot rather than a drawing.",
	         "Paper" ),
	ABSENT( PT_PAPER_G ),
	ABSENT( PT_PAPER_B ),
	TOGGLE( PT_PAPER_FROM_CLIP, "paperFromClip", "Paper from Clip", 0.0f,
	        "Use the clip as the sheet the pen draws on. Effect build only." ),
	SLIDER( PT_GRAIN, "grain", "Grain", 0.15f,
	        "How much of the paper's tooth you can see." ),
	SLIDER( PT_TOOTH, "tooth", "Tooth", 0.222f,
	        "How unevenly the sheet takes ink. A separate question from Grain: smooth "
	        "board takes ink evenly and shows no grain, cartridge does both." ),
	SLIDER( PT_FADE, "fade", "Fade", 0.0f,
	        "How fast the drawing fades. At zero it does not, which is what paper does "
	        "-- this is the one control in the plugin that is not something the machine "
	        "can do, and it is here because a VJ needs the sheet to clear." ),
	OPTION( PT_PRINT, "print", "Print", 0.0f, kPrintNames, kPrintCount,
	        "Ink only ever darkens paper, so a pale line on a dark ground is not "
	        "something the machine can make. A negative of a drawing that could be "
	        "made is, and that is what this is." ),

	//--- Framing -----------------------------------------------------------
	SLIDERG( PT_ZOOM, "size", "Size", 0.610f,
	         "How big the ring is on the sheet. At 1.0 the ring exactly fills the "
	         "height.",
	         "Framing" ),
	SLIDER( PT_CENTRE_X, "centreX", "Centre X", 0.5f, "Where the ring's centre sits." ),
	SLIDER( PT_CENTRE_Y, "centreY", "Centre Y", 0.5f, "Where the ring's centre sits." ),

	//--- Overlay -----------------------------------------------------------
	SLIDERG( PT_GEARS, "showGears", "Show Gears", 0.0f,
	         "Draw the ring, the wheel, the arm and the pen over the top. The teeth are "
	         "the real counts, so they can be counted.",
	         "Overlay" ),
	COLOUR( PT_GEAR_R, "gearTint", "Gear Tint", "The overlay's colour." ),
	ABSENT( PT_GEAR_G ),
	ABSENT( PT_GEAR_B ),

	//--- Output ------------------------------------------------------------
	SLIDERG( PT_MIX, "mix", "Mix", 1.0f,
	         "On the effect build, how much of the drawing is mixed over the clip; at "
	         "zero the clip passes through exactly. On the source build it is the "
	         "opacity.",
	         "Output" ),

	//--- Preset ------------------------------------------------------------
	OPTIONG( PT_PRESET, "preset", "Preset", 1.0f, nullptr, 0,
	         "A whole machine in one gesture. Editing anything a preset covers falls "
	         "back to Custom.",
	         "Preset" ),
};

static_assert( sizeof( kDecls ) / sizeof( kDecls[ 0 ] ) == PT_ABOUT_TEXT,
               "every parameter before the About block needs a Decl, in id order" );

constexpr unsigned int kPresetParamIDs[] = {
	PT_RING, PT_WHEEL, PT_MESH, PT_PEN, PT_SNAP_SET, PT_SNAP_HOLES,
	PT_RATE, PT_DETAIL, PT_CREEP, PT_SKIP, PT_SKIP_TEETH,
	PT_LAYERS, PT_CHANGE, PT_WIPE,
	PT_PEN_SET, PT_PEN_TYPE, PT_INK_R, PT_INK_G, PT_INK_B, PT_FLOW, PT_NIB, PT_SPREAD,
	PT_PAPER_R, PT_PAPER_G, PT_PAPER_B, PT_GRAIN, PT_TOOTH, PT_FADE, PT_PRINT,
	PT_ZOOM, PT_GEARS
};

static_assert( sizeof( kPresetParamIDs ) / sizeof( kPresetParamIDs[ 0 ] ) == presets::kParamCount,
               "the OFX preset id list and presets::Param have gone out of step" );

} // namespace

//===========================================================================
// The plugin.
//===========================================================================
namespace
{
class CogwheelOFXPlugin : public OFX::ImageEffect
{
public:
	CogwheelOFXPlugin( OfxImageEffectHandle handle, bool overInput ) :
		OFX::ImageEffect( handle ),
		over( overInput )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		if( over )
			srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		for( const Decl& d : kDecls )
		{
			params[ d.id ] = d.def;
			switch( d.kind )
			{
			case Kind::Slider: handles[ d.id ] = fetchDoubleParam( d.name ); break;
			case Kind::Count:  handles[ d.id ] = fetchIntParam( d.name ); break;
			case Kind::Toggle: handles[ d.id ] = fetchBooleanParam( d.name ); break;
			case Kind::Option: handles[ d.id ] = fetchChoiceParam( d.name ); break;
			case Kind::Colour: handles[ d.id ] = fetchRGBParam( d.name ); break;
			case Kind::Button:
			case Kind::Absent:
			default: handles[ d.id ] = nullptr; break;
			}
		}

		//The three colours' components hold their FFGL defaults, so that a
		//preset comparison and `Resolve` both see the same numbers whether or
		//not the host has pushed a colour yet.
		for( int j = 0; j < presets::kParamCount; ++j )
			params[ kPresetParamIDs[ j ] ] = presets::kPresets[ 0 ].v[ j ];
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > src;
		if( over && srcClip != nullptr && srcClip->isConnected() )
			src.reset( srcClip->fetchImage( args.time ) );

		if( dst == nullptr )
			OFX::throwSuiteStatusException( kOfxStatFailed );

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();
		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		const OfxRectI bounds = dst->getBounds();
		const int outW        = std::max( 1, bounds.x2 - bounds.x1 );
		const int outH        = std::max( 1, bounds.y2 - bounds.y1 );

		SheetSetup setup;
		simulate( args, outW, outH, src.get(), setup );
		setup.hasClip = over && src != nullptr;

		switch( depth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA
				? run< CogwheelProcessor< unsigned char, 4, 255 > >( args, dst.get(), src.get(), setup )
				: run< CogwheelProcessor< unsigned char, 3, 255 > >( args, dst.get(), src.get(), setup );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA
				? run< CogwheelProcessor< unsigned short, 4, 65535 > >( args, dst.get(), src.get(), setup )
				: run< CogwheelProcessor< unsigned short, 3, 65535 > >( args, dst.get(), src.get(), setup );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA
				? run< CogwheelProcessor< float, 4, 1 > >( args, dst.get(), src.get(), setup )
				: run< CogwheelProcessor< float, 3, 1 > >( args, dst.get(), src.get(), setup );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		if( stoatworks::about::ofx::changedParam( args, paramName ) )
			return;

		if( applyingPreset )
			return;

		const Decl* decl = find( paramName );
		if( decl == nullptr )
			return;

		// ANY edit throws the replay away, not just the obviously structural
		// ones. The drawing on the sheet was laid down under the old values, and
		// continuing it would leave ink on the paper the current settings could
		// never have produced -- which is the sort of difference that only shows
		// up when somebody renders the same timeline twice and gets two files.
		simulatedFrame = kNoFrame;

		if( decl->id == PT_RESET )
			return;

		if( decl->id == PT_PRESET )
		{
			applyPreset( args );
			return;
		}

		// Editing anything a preset covers falls back to Custom. Judged by
		// comparing values rather than by the change reason, so a host echoing
		// our own writes cannot un-set the preset.
		OFX::ChoiceParam* preset = static_cast< OFX::ChoiceParam* >( handles[ PT_PRESET ] );
		int active               = 0;
		preset->getValue( active );
		if( active <= 0 || active > presets::kCount )
			return;

		const presets::Preset& p = presets::kPresets[ active - 1 ];
		readAll( args.time );
		for( int j = 0; j < presets::kParamCount; ++j )
		{
			// A colour's three components all belong to one host parameter, so
			// an edit to it has to be checked against all three.
			const unsigned int id = kPresetParamIDs[ j ];
			if( id != decl->id && !( decl->kind == Kind::Colour && id >= decl->id && id <= decl->id + 2 ) )
				continue;
			if( std::fabs( params[ id ] - p.v[ j ] ) > 1e-6f )
			{
				applyingPreset = true;
				preset->setValue( 0 );
				applyingPreset = false;
				break;
			}
		}
	}

private:
	static constexpr long long kNoFrame = -1000000000LL;

	static const Decl* find( const std::string& name )
	{
		for( const Decl& d : kDecls )
			if( d.kind != Kind::Absent && d.name != nullptr && name == d.name )
				return &d;
		return nullptr;
	}

	template< class PROC >
	void run( const OFX::RenderArguments& args, OFX::Image* dst, OFX::Image* src, const SheetSetup& setup )
	{
		PROC processor( *this );
		processor.setDstImg( dst );
		processor.setSetup( &setup, src );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	//-----------------------------------------------------------------------
	// Reading the host's controls back into the 0..1 array Controls.cpp wants.
	//
	// The FFGL build holds exactly this array and hands it to `Resolve`; doing
	// the same here is what makes the mapping -- every tooth count, every turn
	// per second, every absorption -- shared rather than mirrored.
	//-----------------------------------------------------------------------
	void readAll( double time )
	{
		for( const Decl& d : kDecls )
		{
			switch( d.kind )
			{
			case Kind::Slider:
				params[ d.id ] = static_cast< float >(
					static_cast< OFX::DoubleParam* >( handles[ d.id ] )->getValueAtTime( time ) );
				break;
			case Kind::Count:
				params[ d.id ] = static_cast< float >(
					static_cast< OFX::IntParam* >( handles[ d.id ] )->getValueAtTime( time ) );
				break;
			case Kind::Toggle:
				params[ d.id ] =
					static_cast< OFX::BooleanParam* >( handles[ d.id ] )->getValueAtTime( time ) ? 1.0f : 0.0f;
				break;
			case Kind::Option:
			{
				int chosen = 0;
				static_cast< OFX::ChoiceParam* >( handles[ d.id ] )->getValueAtTime( time, chosen );
				//An option holds its element VALUE, which for every dropdown
				//here is its index. Getting this backwards gives a control
				//permanently stuck on its first entry.
				params[ d.id ] = static_cast< float >( chosen );
				break;
			}
			case Kind::Colour:
			{
				double r = 0.0, g = 0.0, b = 0.0;
				static_cast< OFX::RGBParam* >( handles[ d.id ] )->getValueAtTime( time, r, g, b );
				params[ d.id + 0 ] = static_cast< float >( r );
				params[ d.id + 1 ] = static_cast< float >( g );
				params[ d.id + 2 ] = static_cast< float >( b );
				break;
			}
			case Kind::Button:
			case Kind::Absent:
			default:
				break;
			}
		}
	}

	void applyPreset( const OFX::InstanceChangedArgs& args )
	{
		OFX::ChoiceParam* preset = static_cast< OFX::ChoiceParam* >( handles[ PT_PRESET ] );
		int chosen               = 0;
		preset->getValue( chosen );
		if( chosen <= 0 || chosen > presets::kCount )
			return;

		const presets::Preset& p = presets::kPresets[ chosen - 1 ];

		applyingPreset = true;
		beginEditBlock( "preset" );
		for( int j = 0; j < presets::kParamCount; ++j )
		{
			const unsigned int id = kPresetParamIDs[ j ];
			const Decl& d         = kDecls[ id ];
			switch( d.kind )
			{
			case Kind::Slider:
				static_cast< OFX::DoubleParam* >( handles[ id ] )->setValue( p.v[ j ] );
				break;
			case Kind::Count:
				static_cast< OFX::IntParam* >( handles[ id ] )->setValue(
					static_cast< int >( std::lround( p.v[ j ] ) ) );
				break;
			case Kind::Toggle:
				static_cast< OFX::BooleanParam* >( handles[ id ] )->setValue( p.v[ j ] > 0.5f );
				break;
			case Kind::Option:
				static_cast< OFX::ChoiceParam* >( handles[ id ] )->setValue(
					static_cast< int >( std::lround( p.v[ j ] ) ) );
				break;
			case Kind::Colour:
				//The three components are consecutive ids in the table, in the
				//same order the FFGL build declares them.
				static_cast< OFX::RGBParam* >( handles[ id ] )->setValue(
					p.v[ j ], p.v[ j + 1 ], p.v[ j + 2 ] );
				break;
			case Kind::Absent:
			case Kind::Button:
			default:
				break;
			}
		}
		endEditBlock();
		applyingPreset = false;

		( void )args;
		simulatedFrame = kNoFrame;
	}

	//-----------------------------------------------------------------------
	// The replay. See the header comment.
	//-----------------------------------------------------------------------
	void simulate( const OFX::RenderArguments& args, int outW, int outH,
	               OFX::Image* src, SheetSetup& setup )
	{
		// OFX hands render time in frames. A host that reports no frame rate
		// gets 25 -- wrong somewhere, but never zero, which would make every
		// frame the first one.
		double fps = dstClip != nullptr ? dstClip->getFrameRate() : 0.0;
		if( !( fps > 0.0 ) )
			fps = 25.0;

		// Clamped the way Clock::Update clamps its own delta, and for the same
		// reason.
		const double frameSeconds = std::clamp( 1.0 / fps, 1.0 / 240.0, 1.0 / 24.0 );

		const long long wantFrame = static_cast< long long >( std::llround( args.time ) );

		paper.ensure( outW, outH );

		const bool resized = ( sheetW != outW || sheetH != outH );
		sheetW             = outW;
		sheetH             = outH;

		//Back to the beginning, not to a warm-up window. A drawing has no
		//forgetting time to bound a window with: everything laid down since the
		//sheet was last wiped is still on it.
		const bool restart = simulatedFrame == kNoFrame || wantFrame < simulatedFrame || resized;
		if( restart )
		{
			paper.clear();
			readAll( args.time );
			crank.Restart( static_cast< uint32_t >(
				std::max( 1, static_cast< int >( std::lround( params[ PT_SEED ] ) ) ) ) );
			simulatedFrame = -1;
		}

		const float aspect = static_cast< float >( outW ) / static_cast< float >( outH );

		Resolved resolved = Resolve( params, 120.0, over );
		Geometry geometry;

		for( long long frame = simulatedFrame + 1; frame <= wantFrame; ++frame )
		{
			//Each step reads the controls at its OWN time rather than holding
			//the requested frame's values through the replay. That costs a few
			//dozen host parameter reads a frame and it buys the thing the
			//plugin is about: a keyframed Crank arrives at this frame having
			//actually been cranked at the rates the operator drew.
			readAll( static_cast< double >( frame ) );
			resolved = Resolve( params, 120.0, over );

			geometry = crank.Advance( resolved.crank, frameSeconds, steps, runs );

			if( crank.WipeRequested() )
			{
				paper.clear();
				crank.ClearWipeRequest();
			}

			if( resolved.render.fadeSeconds > 0.0f )
				paper.fade( std::exp( -static_cast< float >( frameSeconds ) / resolved.render.fadeSeconds ) );

			DepositParams deposit;
			deposit.flow         = resolved.render.flow;
			deposit.perDistance  = resolved.render.perDistance;
			deposit.nibSigma     = resolved.render.nibSigma;
			deposit.nibSpread    = resolved.render.nibSpread;
			deposit.densityFloor = resolved.render.densityFloor;
			deposit.scale        = resolved.render.scale;
			deposit.centre[ 0 ]  = resolved.render.centre[ 0 ];
			deposit.centre[ 1 ]  = resolved.render.centre[ 1 ];
			deposit.tooth        = resolved.render.tooth;
			deposit.toothScale   = resolved.render.toothScale;
			deposit.aspect       = aspect;

			for( const Run& run : runs )
			{
				for( int i = run.first; i + 1 < run.first + run.count; ++i )
				{
					float colour[ 3 ] = { run.colour[ 0 ], run.colour[ 1 ], run.colour[ 2 ] };
					if( resolved.render.inkFromClip && src != nullptr )
						sampleClip( src, steps[ i ], deposit, outW, outH, colour );

					float absorb[ 3 ];
					Absorption( colour, absorb );
					depositSegment( paper, steps[ i ], steps[ i + 1 ], absorb, deposit );
				}
			}
		}

		simulatedFrame = wantFrame;

		//---------------------------------------------------------------
		// What the sheet pass needs. Every gear number comes from the SAME
		// Geometry and the SAME theta the deposit above used, rather than
		// being recomputed -- two copies of the gear maths drift, and the
		// symptom is an overlay a fraction of a turn away from the line it is
		// supposed to be making.
		//---------------------------------------------------------------
		setup.paper  = &paper;
		setup.aspect = aspect;
		for( int c = 0; c < 3; ++c )
		{
			setup.paperColour[ c ] = resolved.render.paperColour[ c ];
			setup.gearColour[ c ]  = resolved.render.gearColour[ c ];
		}
		setup.paperFromClip = resolved.render.paperFromClip;
		setup.paperGrain    = resolved.render.paperGrain;
		setup.toothScale    = resolved.render.toothScale;
		setup.negative      = resolved.render.negative;
		setup.opacity       = resolved.render.opacity;
		setup.passthrough   = over ? resolved.render.passthrough : 0.0f;
		setup.gearLevel     = resolved.render.gearLevel;
		setup.scale         = resolved.render.scale;
		setup.centre[ 0 ]   = resolved.render.centre[ 0 ];
		setup.centre[ 1 ]   = resolved.render.centre[ 1 ];

		setup.ringTeeth   = static_cast< float >( geometry.ringTeeth );
		setup.wheelTeeth  = static_cast< float >( geometry.wheelTeeth );
		setup.wheelRadius = static_cast< float >( geometry.wheelRadius );

		const double theta = crank.Theta();
		setup.wheelCentre[ 0 ] = static_cast< float >( geometry.orbit * std::cos( theta ) );
		setup.wheelCentre[ 1 ] = static_cast< float >( geometry.orbit * std::sin( theta ) );
		setup.wheelAngle       = static_cast< float >( WheelAngle( geometry, theta, crank.SlipTeeth() ) );

		const PenPoint pen  = PenAt( geometry, theta, crank.SlipTeeth() );
		setup.penPoint[ 0 ] = static_cast< float >( pen.x );
		setup.penPoint[ 1 ] = static_cast< float >( pen.y );
	}

	/// The clip's colour where the nib is. Mirrors the ink shader's `segClipUV`
	/// -- the MIDPOINT of the interval, not a fragment position: the pen picks
	/// up what it is standing on, not what is under the far edge of the blot it
	/// is about to make.
	void sampleClip( OFX::Image* src, const Step& step, const DepositParams& p,
	                 int outW, int outH, float colour[ 3 ] ) const
	{
		const float sx = step.x * p.scale + p.centre[ 0 ];
		const float sy = step.y * p.scale + p.centre[ 1 ];

		const int x = std::clamp(
			static_cast< int >( ( sx / ( 2.0f * p.aspect ) + 0.5f ) * static_cast< float >( outW ) ),
			0, outW - 1 );
		const int y = std::clamp(
			static_cast< int >( ( sy * 0.5f + 0.5f ) * static_cast< float >( outH ) ), 0, outH - 1 );

		const OfxRectI bounds = src->getBounds();
		const int px          = std::clamp( x + bounds.x1, bounds.x1, bounds.x2 - 1 );
		const int py          = std::clamp( y + bounds.y1, bounds.y1, bounds.y2 - 1 );

		//Read at the image's own depth. A float clip and an 8-bit clip are the
		//same picture and must give the same pen.
		switch( src->getPixelDepth() )
		{
		case OFX::eBitDepthUByte:
		{
			const unsigned char* q = static_cast< const unsigned char* >( src->getPixelAddress( px, py ) );
			if( q != nullptr )
				for( int c = 0; c < 3; ++c )
					colour[ c ] = static_cast< float >( q[ c ] ) / 255.0f;
			break;
		}
		case OFX::eBitDepthUShort:
		{
			const unsigned short* q = static_cast< const unsigned short* >( src->getPixelAddress( px, py ) );
			if( q != nullptr )
				for( int c = 0; c < 3; ++c )
					colour[ c ] = static_cast< float >( q[ c ] ) / 65535.0f;
			break;
		}
		case OFX::eBitDepthFloat:
		{
			const float* q = static_cast< const float* >( src->getPixelAddress( px, py ) );
			if( q != nullptr )
				for( int c = 0; c < 3; ++c )
					colour[ c ] = std::clamp( q[ c ], 0.0f, 1.0f );
			break;
		}
		default:
			break;
		}
	}

	const bool over;
	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::Param* handles[ PT_COUNT ] = {};
	float params[ PT_COUNT ]        = {};

	Paper paper;
	Crank crank;
	std::vector< Step > steps;
	std::vector< Run > runs;

	long long simulatedFrame = kNoFrame;
	int sheetW = 0, sheetH = 0;
	bool applyingPreset = false;
};

//===========================================================================
// Describe.
//===========================================================================
void describeCommon( OFX::ImageEffectDescriptor& desc, const char* name )
{
	desc.setLabels( name, name, name );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// Tiles are declined because the sheet is a whole-frame accumulation
	// before any pixel of the output exists, and because a tile does not know
	// the frame's geometry -- and the pen is placed in the sheet's own
	// coordinates.
	desc.setSupportsTiles( false );
	desc.setSupportsMultiResolution( true );

	// The replay carries the drawing from frame to frame and is therefore not
	// temporal clip access: the plugin never reads another frame's image, it
	// re-runs its own machine. Saying otherwise would make a host fetch images
	// nothing looks at.
	desc.setTemporalClipAccess( false );

	// Instance safe, not fully safe. The sheet and the crank are per-instance
	// mutable state, and two concurrent renders of the same instance would be
	// two threads drawing on the same paper. The pixel pass underneath is still
	// multi-threaded, which is where the pixels are.
	desc.setRenderThreadSafety( OFX::eRenderInstanceSafe );
}

void describeParams( OFX::ImageEffectDescriptor& desc, bool overVariant )
{
	OFX::PageParamDescriptor* page   = desc.definePageParam( "Controls" );
	OFX::GroupParamDescriptor* group = nullptr;

	for( const Decl& d : kDecls )
	{
		if( d.kind == Kind::Absent )
			continue;

		if( d.group != nullptr )
		{
			group = desc.defineGroupParam( d.group );
			group->setLabels( d.group, d.group, d.group );
			page->addChild( *group );
		}

		OFX::ParamDescriptor* param = nullptr;

		switch( d.kind )
		{
		case Kind::Slider:
		{
			OFX::DoubleParamDescriptor* p = desc.defineDoubleParam( d.name );
			p->setRange( 0.0, 1.0 );
			p->setDisplayRange( 0.0, 1.0 );
			p->setDefault( d.def );
			param = p;
			break;
		}
		case Kind::Count:
		{
			OFX::IntParamDescriptor* p = desc.defineIntParam( d.name );
			p->setRange( static_cast< int >( d.lo ), static_cast< int >( d.hi ) );
			p->setDisplayRange( static_cast< int >( d.lo ), static_cast< int >( d.hi ) );
			p->setDefault( static_cast< int >( std::lround( d.def ) ) );
			param = p;
			break;
		}
		case Kind::Toggle:
		{
			OFX::BooleanParamDescriptor* p = desc.defineBooleanParam( d.name );
			p->setDefault( d.def > 0.5f );
			param = p;
			break;
		}
		case Kind::Option:
		{
			OFX::ChoiceParamDescriptor* p = desc.defineChoiceParam( d.name );
			if( d.id == PT_PRESET )
			{
				//Element 0 is Custom and is not in the table: it means the
				//sliders are the truth.
				p->appendOption( "Custom" );
				for( int i = 0; i < presets::kCount; ++i )
					p->appendOption( presets::kPresets[ i ].name );
			}
			else
			{
				for( int i = 0; i < d.optionCount; ++i )
					p->appendOption( d.options[ i ] );
			}
			p->setDefault( static_cast< int >( std::lround( d.def ) ) );
			param = p;
			break;
		}
		case Kind::Colour:
		{
			OFX::RGBParamDescriptor* p = desc.defineRGBParam( d.name );
			//The default comes out of the preset table, so a colour and its
			//three FFGL components cannot open on different values.
			int slot = -1;
			for( int j = 0; j < presets::kParamCount; ++j )
				if( kPresetParamIDs[ j ] == d.id )
					slot = j;
			if( slot >= 0 )
				p->setDefault( presets::kPresets[ 0 ].v[ slot ],
				               presets::kPresets[ 0 ].v[ slot + 1 ],
				               presets::kPresets[ 0 ].v[ slot + 2 ] );
			param = p;
			break;
		}
		case Kind::Button:
		{
			param = desc.definePushButtonParam( d.name );
			break;
		}
		default:
			break;
		}

		if( param == nullptr )
			continue;

		param->setLabels( d.label, d.label, d.label );
		if( d.hint != nullptr && d.hint[ 0 ] != '\0' )
			param->setHint( d.hint );
		if( group != nullptr )
			param->setParent( *group );
		page->addChild( *param );
	}

	( void )overVariant;

	// The Stoatworks About block: a read-only credit line and one push button
	// per link, in a group that starts folded. Last, so it sits under the
	// plugin's own controls.
	stoatworks::about::ofx::describe( desc, page );
}

} // namespace

//---------------------------------------------------------------------------
// "Cogwheel": the generator.
//---------------------------------------------------------------------------
mDeclarePluginFactory( CogwheelSourceFactory, {}, {} );

void CogwheelSourceFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	describeCommon( desc, "Cogwheel" );
	desc.addSupportedContext( OFX::eContextGenerator );
	desc.addSupportedContext( OFX::eContextGeneral );
}

void CogwheelSourceFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	describeParams( desc, false );
}

OFX::ImageEffect* CogwheelSourceFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new CogwheelOFXPlugin( handle, false );
}

//---------------------------------------------------------------------------
// "Cogwheel Ink": the effect. The clip can be the paper the pen draws on, the
// ink the pen picks up, or both.
//---------------------------------------------------------------------------
mDeclarePluginFactory( CogwheelInkFactory, {}, {} );

void CogwheelInkFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	describeCommon( desc, "Cogwheel Ink" );
	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );
}

void CogwheelInkFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	describeParams( desc, true );
}

OFX::ImageEffect* CogwheelInkFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new CogwheelOFXPlugin( handle, true );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static CogwheelSourceFactory* sourceFactory =
		new CogwheelSourceFactory( kSourceIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	static CogwheelInkFactory* inkFactory =
		new CogwheelInkFactory( kInkIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( sourceFactory );
	ids.push_back( inkFactory );
}
