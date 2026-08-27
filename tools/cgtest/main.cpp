/**
    cgtest -- the offline harness.

    It drives **the real code that ships** in a headless core-profile context:
    `CogwheelPlugin` for anything that goes through the parameter list, and
    `Sheet` directly for the two invariants that need a pen path the harness
    controls to the last `dt` rather than one the crank happened to produce.
    Nothing below is a reimplementation and nothing below is a preview -- every
    number printed comes out of a frame that was actually rendered, or out of
    the same `Solve()` the plugin calls.

        --closure   the gear arithmetic: lobes, turns, and the pen coming home
        --detail    total ink is the same at Draft, Normal and Fine
        --rate      the same second of cranking at 24, 60 and 120 fps
        --beer      two pens crossing multiply, they do not add
        --liveness  the defaults keep drawing; a stiff machine provably stops
        --presets   every preset draws something with structure in it
        --defaults  the constructor's defaults ARE preset 1
        --hosts     presets survive all three host behaviours
        --scale     the same preset is the same drawing at every raster
        --guard     a hostile machine leaves no NaN on the sheet
        --all       every one of the above, with a summary

        --out PATH  render a frame     --size WxH   --frames N   --preset N
        --contact P a sheet of every preset
        --list      every parameter    --names   --set ID=V (or "Name=V")
        --effect    drive the effect build over a test clip

    ## Determinism

    **Time comes from the frame counter and never from a wall clock.** `Clock`
    falls back to `steady_clock` when the host has never called `SetTime`, so
    every render below calls `SetTime( frame / fps )` first -- which also pins
    `FrameSeconds` through the clamp. Two runs of the same command produce
    byte-identical PNGs.

    A synthetic transport goes with it: 120 BPM in 4/4 from time zero, so a bar
    is exactly two seconds and the Sync switch has something real to divide.
    Left at the SDK's defaults, `bpm` is whatever the base class was constructed
    with and Sync would look dead.

    ## Why every frame goes through `ProcessOpenGL`

    There is no back door and there should not be one. `ProcessOpenGL` is the
    call a host makes, and it is the only path that advances the clock and the
    crank -- so it is the only path on which the drawing exists at all.
    `InitGL` is called once per instance rather than per frame, because
    `Sheet::InitGL` compiles three shaders and generates a VAO and a VBO
    without deleting the previous set.

    ## Measuring ink

    Several checks below want "how much ink went on the sheet", and the sheet
    does not hold ink -- it holds optical density, and what comes out of the
    display pass is `paper * exp( -density )`. So the measurement configuration
    is always the same: **white paper, one grey pen, no grain, no tooth, no
    fade**, after which every pixel's density is `-ln( shown )` exactly and the
    frame's total is a sum. A grey pen and not a black one: a black pen's
    absorption is floored at 9.2 per channel and a few strokes take `shown`
    down where a float has stopped being able to tell one density from another.

    Grain and tooth are off for a different reason. Both are mean-preserving,
    so they do not bias a total -- but they add variance, and these checks
    quote spreads of a fraction of a percent.

    ## What each test can and cannot catch

    `--closure` is the test the plugin exists to pass and it needs no GL at all.
    It checks the published arithmetic (lobes = N/gcd, turns = n/gcd) against an
    independent gcd, and then checks the arithmetic against the *machine*: it
    walks `PenAt` for the full number of turns and asserts the pen came back to
    where it started. Those are different claims -- the first is about the
    formula, the second is about the formula describing this particular
    implementation of the gears -- and only the second would catch a sign error
    in `spinPerOrbit`.

    `--detail` is the ink-conservation identity. It cannot say the drawing looks
    right; it says the closed form in the ink shader integrates to the same
    total however finely the path is chopped, which is what makes Detail a
    quality control rather than a brightness control.

    `--beer` is a parameter-free identity and the sharpest test here. Where two
    strokes cross, the transmitted light must be the *product* of what each
    stroke transmits on its own, divided by the paper -- with no reference to
    flow, nib, speed or colour. Additive blending fails it by a mile and so
    does any renderer that composites a stroke as an alpha layer.

    `--liveness` fails if anybody makes the dead machine the default. It also
    proves the dead machine is real, which matters: without the second half a
    reader would reasonably assume the closure death is a theoretical worry.

    None of them catches a uniform whose name does not match the GLSL, because
    `glUniform` at location -1 is a documented no-op. See `tools/sweep.py`.
*/

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Cogwheel.h"
#include "Controls.h"
#include "Presets.h"
#include "machine/Gears.h"
#include "machine/Wheels.h"
#include "render/Sheet.h"

using namespace cogwheel;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );// filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

	std::vector< unsigned char > header;
	putU32( header, static_cast< uint32_t >( width ) );
	putU32( header, static_cast< uint32_t >( height ) );
	header.push_back( 8 );// bit depth
	header.push_back( 6 );// colour type: RGBA
	header.push_back( 0 );
	header.push_back( 0 );
	header.push_back( 0 );
	putChunk( png, "IHDR", header );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	std::FILE* file = std::fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

struct Target
{
	GLuint texture = 0;
	GLuint fbo     = 0;
	int width      = 0;
	int height     = 0;
};

/// RGBA32F always.
///
/// Not a luxury. The checks below recover a density as `-ln( shown )`, and
/// eight bits per channel cannot represent a transmission below 1/255 at all --
/// which is a density of only 5.5, well inside what a few crossing strokes
/// reach. An RGBA8 target would report a saturated crossing as pure black and
/// the Beer's-law identity would pass trivially wherever it mattered most.
Target makeTarget( int width, int height )
{
	Target target;
	target.width  = width;
	target.height = height;

	glGenTextures( 1, &target.texture );
	glBindTexture( GL_TEXTURE_2D, target.texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenFramebuffers( 1, &target.fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0 );

	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
		std::fprintf( stderr, "the %dx%d target is not framebuffer-complete\n", width, height );
	return target;
}

void releaseTarget( Target& target )
{
	if( target.fbo != 0 )
		glDeleteFramebuffers( 1, &target.fbo );
	if( target.texture != 0 )
		glDeleteTextures( 1, &target.texture );
	target = Target();
}

/// Straight out of GL, **bottom row first**.
std::vector< float > readFloats( const Target& target )
{
	std::vector< float > pixels( static_cast< size_t >( target.width ) * target.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.width, target.height, GL_RGBA, GL_FLOAT, pixels.data() );
	return pixels;
}

/// Bottom-up float RGBA to top-down 8-bit RGBA, for the PNG writer.
std::vector< unsigned char > toBytes( const std::vector< float >& bottomUp, int width, int height )
{
	std::vector< unsigned char > out( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
	{
		const float* src = bottomUp.data() + static_cast< size_t >( height - 1 - y ) * width * 4;
		unsigned char* dst = out.data() + static_cast< size_t >( y ) * width * 4;
		for( int x = 0; x < width * 4; ++x )
			dst[ x ] = static_cast< unsigned char >(
				std::lround( std::clamp( src[ x ], 0.0f, 1.0f ) * 255.0f ) );
	}
	return out;
}

/// One pixel, in frame coordinates (0..1, y down).
void samplePixel( const std::vector< float >& bottomUp, int width, int height,
                  float fx, float fy, float rgb[ 3 ] )
{
	const int x     = std::clamp( static_cast< int >( fx * static_cast< float >( width ) ), 0, width - 1 );
	const int yDown = std::clamp( static_cast< int >( fy * static_cast< float >( height ) ), 0, height - 1 );
	const int y     = height - 1 - yDown;

	const float* p = bottomUp.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
	rgb[ 0 ] = p[ 0 ];
	rgb[ 1 ] = p[ 1 ];
	rgb[ 2 ] = p[ 2 ];
}

/// Total optical density on a sheet of white paper, summed over every pixel.
///
/// The frame has to have been rendered in the measurement configuration -- see
/// the note at the top of this file -- or this number is not a density.
double totalDensity( const std::vector< float >& frame )
{
	double sum = 0.0;
	for( size_t i = 0; i < frame.size(); i += 4 )
	{
		const double shown = std::max( static_cast< double >( frame[ i ] ), 1.0e-12 );
		if( shown < 1.0 )
			sum += -std::log( shown );
	}
	return sum;
}

/// A frame's per-pixel optical density, one channel, on white paper.
///
/// The frame has to have been rendered in the measurement configuration or
/// this is not a density. Averaging densities is meaningful and averaging
/// what the display pass shows is not: `shown` is an exponential of the
/// density, so the mean of two shown values is not the shown value of their
/// mean -- which is exactly the trap a comparison between two rasters falls
/// into, because a line narrower than a pixel is dark in the middle of one
/// pixel at 320 wide and spread over eight at 1280.
std::vector< float > densityImage( const std::vector< float >& frame )
{
	std::vector< float > out( frame.size() / 4 );
	for( size_t i = 0, o = 0; i < frame.size(); i += 4, ++o )
	{
		const double shown = std::clamp( static_cast< double >( frame[ i ] ), 1.0e-9, 1.0 );
		out[ o ] = static_cast< float >( -std::log( shown ) );
	}
	return out;
}

/// Box-average a single-channel image down to `cols` x `rows`.
///
/// A box average and not a resample: the question being asked is how much ink
/// is in each patch of sheet, and a box is the only filter that answers it
/// without inventing or discarding any.
std::vector< double > boxDown( const std::vector< float >& image, int width, int height,
                               int cols, int rows )
{
	std::vector< double > out( static_cast< size_t >( cols ) * rows, 0.0 );
	std::vector< double > counts( out.size(), 0.0 );

	for( int y = 0; y < height; ++y )
	{
		const int row = std::min( rows - 1, y * rows / height );
		for( int x = 0; x < width; ++x )
		{
			const int column = std::min( cols - 1, x * cols / width );
			const size_t cell = static_cast< size_t >( row ) * cols + column;
			out[ cell ] += image[ static_cast< size_t >( y ) * width + x ];
			counts[ cell ] += 1.0;
		}
	}

	for( size_t i = 0; i < out.size(); ++i )
		if( counts[ i ] > 0.0 )
			out[ i ] /= counts[ i ];
	return out;
}

/// How different two frames are, as a mean absolute difference in luma.
double frameDifference( const std::vector< float >& a, const std::vector< float >& b )
{
	if( a.size() != b.size() || a.empty() )
		return 1.0;

	double sum = 0.0;
	size_t n   = 0;
	for( size_t i = 0; i < a.size(); i += 4 )
	{
		const double la = 0.299 * a[ i ] + 0.587 * a[ i + 1 ] + 0.114 * a[ i + 2 ];
		const double lb = 0.299 * b[ i ] + 0.587 * b[ i + 1 ] + 0.114 * b[ i + 2 ];
		sum += std::fabs( la - lb );
		++n;
	}
	return n > 0 ? sum / static_cast< double >( n ) : 1.0;
}

//---------------------------------------------------------------------------
// Driving the plugin.
//---------------------------------------------------------------------------
bool startPlugin( CogwheelPlugin& plugin, const Target& target )
{
	FFGLViewportStruct viewport {};
	viewport.width  = static_cast< FFUInt32 >( target.width );
	viewport.height = static_cast< FFUInt32 >( target.height );

	// Once per instance, never per frame: `Sheet::InitGL` compiles three
	// shaders and generates a VAO and a VBO without deleting the previous set,
	// so calling it every frame leaks a program and two objects per frame.
	return plugin.InitGL( &viewport ) == FF_SUCCESS;
}

bool renderFrame( CogwheelPlugin& plugin, const Target& target, int frameIndex,
                  double fps = 60.0, GLuint clip = 0 )
{
	// The whole of the harness's determinism is these two lines. `Clock::Update`
	// takes the wall clock only when the host has never called SetTime, so
	// driving it from the frame counter is what stops the drawing depending on
	// how long the process has been alive.
	const double seconds = static_cast< double >( frameIndex ) / fps;
	plugin.SetClockScaleForTest( 1.0 );//seconds, said out loud rather than inferred
	plugin.SetTime( seconds );

	// 120 BPM in 4/4 from time zero, so bar N starts at exactly 2N seconds.
	constexpr double kBarSeconds = 2.0;
	plugin.SetBeatInfo( 120.0f, static_cast< float >( std::fmod( seconds, kBarSeconds ) / kBarSeconds ) );

	FFGLTextureStruct inputStruct {};
	inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( target.width );
	inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( target.height );
	inputStruct.Handle                              = clip;
	FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

	ProcessOpenGLStruct process {};
	process.numInputTextures = clip != 0 ? 1 : 0;
	process.inputTextures    = clip != 0 ? inputs : nullptr;
	process.HostFBO          = target.fbo;

	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glViewport( 0, 0, target.width, target.height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
}

/// Every parameter's host-facing name, read out of the plugin itself.
///
/// Built at runtime rather than kept as a table beside Controls.h, and that is
/// not tidiness: a hand-written table is a second place for a name to live, and
/// the failure it produces is a `--set` that silently addresses nothing while
/// everything else about the run looks correct.
std::map< std::string, unsigned int > parameterIndex( CogwheelPlugin& plugin )
{
	std::map< std::string, unsigned int > byName;
	for( unsigned int id = 0; id < PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name != nullptr && name[ 0 ] != '\0' )
			byName[ name ] = id;
	}
	return byName;
}

int applySets( CogwheelPlugin& plugin, const std::vector< std::pair< std::string, float > >& sets )
{
	const std::map< std::string, unsigned int > byName = parameterIndex( plugin );
	int unresolved = 0;

	for( const auto& set : sets )
	{
		char* end           = nullptr;
		const long asNumber = std::strtol( set.first.c_str(), &end, 10 );
		unsigned int id     = PT_COUNT;

		if( end != nullptr && *end == '\0' && !set.first.empty() && asNumber >= 0 && asNumber < PT_COUNT )
			id = static_cast< unsigned int >( asNumber );
		else
		{
			const auto found = byName.find( set.first );
			if( found != byName.end() )
				id = found->second;
		}

		if( id >= PT_COUNT )
		{
			std::fprintf( stderr, "cgtest: no parameter called \"%s\"\n", set.first.c_str() );
			++unresolved;
			continue;
		}
		plugin.SetFloatParameter( id, set.second );
	}

	return unresolved;
}

/// The measurement configuration: white paper, one grey pen, nothing textured.
/// See the note at the top of this file for why each of these is here.
void setMeasurementLook( CogwheelPlugin& plugin )
{
	plugin.SetFloatParameter( PT_PRESET, 0.0f );//Custom: these values are the truth
	plugin.SetFloatParameter( PT_PEN_SET, static_cast< float >( PenSet::Ink ) );
	plugin.SetFloatParameter( PT_INK_R, 0.5f );
	plugin.SetFloatParameter( PT_INK_G, 0.5f );
	plugin.SetFloatParameter( PT_INK_B, 0.5f );
	plugin.SetFloatParameter( PT_PAPER_R, 1.0f );
	plugin.SetFloatParameter( PT_PAPER_G, 1.0f );
	plugin.SetFloatParameter( PT_PAPER_B, 1.0f );
	plugin.SetFloatParameter( PT_GRAIN, 0.0f );
	plugin.SetFloatParameter( PT_TOOTH, 0.0f );
	plugin.SetFloatParameter( PT_FADE, 0.0f );
	plugin.SetFloatParameter( PT_GEARS, 0.0f );
	plugin.SetFloatParameter( PT_PRINT, 0.0f );
	plugin.SetFloatParameter( PT_MIX, 1.0f );
	plugin.SetFloatParameter( PT_SYNC, static_cast< float >( Sync::Free ) );
}

//---------------------------------------------------------------------------
// --closure
//---------------------------------------------------------------------------
int runClosure()
{
	std::printf( "closure -- the gear arithmetic, and the pen coming home\n\n" );
	std::printf( "  ring wheel mesh    gcd  lobes  turns   pen returns to within\n" );

	struct Case
	{
		int ring;
		int wheel;
		Mesh mesh;
	};
	const Case cases[] = {
		{ 96, 32, Mesh::Inside },  //3:1 exactly -- one turn, three lobes
		{ 96, 52, Mesh::Inside },  //the classic
		{ 96, 31, Mesh::Inside },  //coprime: 31 turns, 96 lobes
		{ 105, 64, Mesh::Inside },
		{ 144, 60, Mesh::Inside },
		{ 96, 45, Mesh::Outside },
		{ 105, 40, Mesh::Outside },
		{ 150, 24, Mesh::Inside },
	};

	int failures = 0;
	for( const Case& c : cases )
	{
		Train train;
		train.ringTeeth   = c.ring;
		train.wheelTeeth  = c.wheel;
		train.mesh        = c.mesh;
		train.penFraction = 0.7;

		const Geometry g = Solve( train );
		const int gcd    = Gcd( c.ring, c.wheel );

		// The formula, against an independent gcd.
		const bool lobesOk = ( g.lobes == c.ring / gcd );
		const bool turnsOk = ( g.turnsToClose == c.wheel / gcd );

		// The formula, against the MACHINE. Walking PenAt for the full number
		// of turns and asking whether the pen came home is a different claim
		// from the one above, and it is the only one of the two that would
		// catch a sign error in spinPerOrbit.
		const PenPoint start = PenAt( g, 0.0, 0.0 );
		const PenPoint end   = PenAt( g, 6.283185307179586 * g.turnsToClose, 0.0 );
		const double gap     = std::hypot( end.x - start.x, end.y - start.y );

		// A quarter of a thousandth of the ring's radius. Not a float epsilon:
		// the angle is a product of a double by an integer turn count, so the
		// error is rounding in the trig and nothing else.
		const bool homeOk = gap < 2.5e-4;

		const bool ok = lobesOk && turnsOk && homeOk;
		if( !ok )
			++failures;

		std::printf( "  %4d %5d %-7s %4d %6d %6d   %10.2e  %s\n",
		             c.ring, c.wheel, c.mesh == Mesh::Inside ? "inside" : "outside",
		             gcd, g.lobes, g.turnsToClose, gap, ok ? "ok" : "FAIL" );
	}

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --detail
//---------------------------------------------------------------------------
int runDetail( const Target& target )
{
	std::printf( "detail -- the same drawing, walked at three step rates\n\n" );

	double totals[ kDetailCount ] = {};

	for( int d = 0; d < kDetailCount; ++d )
	{
		CogwheelPlugin plugin( false );
		if( !startPlugin( plugin, target ) )
			return 1;

		setMeasurementLook( plugin );
		plugin.SetFloatParameter( PT_DETAIL, static_cast< float >( d ) );
		plugin.SetFloatParameter( PT_LAYERS, 1.0f );
		plugin.SetFloatParameter( PT_CHANGE, static_cast< float >( Change::Nothing ) );
		plugin.SetFloatParameter( PT_CREEP, 0.0f );
		plugin.SetFloatParameter( PT_SKIP, 0.0f );
		//A nib several pixels wide, so that the quantisation of a stroke onto
		//the raster is not what the measurement is dominated by.
		plugin.SetFloatParameter( PT_NIB, 0.5f );

		for( int frame = 0; frame < 30; ++frame )
			renderFrame( plugin, target, frame );

		totals[ d ] = totalDensity( readFloats( target ) );
		plugin.DeInitGL();

		std::printf( "  %-7s %14.1f\n", kDetailNames[ d ], totals[ d ] );
	}

	const double lo = std::min( { totals[ 0 ], totals[ 1 ], totals[ 2 ] } );
	const double hi = std::max( { totals[ 0 ], totals[ 1 ], totals[ 2 ] } );
	const double spread = hi > 0.0 ? ( hi - lo ) / hi : 1.0;

	//Half a percent. The closed form conserves ink exactly, so what is left is
	//the raster: a stroke drawn as 360 segments and the same stroke drawn as
	//5760 land on slightly different sets of texels near a cusp, where the path
	//curves inside one segment.
	const bool ok = spread < 0.005;
	std::printf( "\n  spread %.3f%%  %s\n", 100.0 * spread, ok ? "PASS" : "FAIL" );
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
// --rate
//---------------------------------------------------------------------------
int runRate( const Target& target )
{
	std::printf( "rate -- one second of cranking, delivered at three frame rates\n\n" );

	const double rates[] = { 24.0, 60.0, 120.0 };
	double totals[ 3 ]   = {};

	for( int i = 0; i < 3; ++i )
	{
		CogwheelPlugin plugin( false );
		if( !startPlugin( plugin, target ) )
			return 1;

		setMeasurementLook( plugin );
		plugin.SetFloatParameter( PT_LAYERS, 1.0f );
		plugin.SetFloatParameter( PT_CHANGE, static_cast< float >( Change::Nothing ) );
		plugin.SetFloatParameter( PT_CREEP, 0.0f );
		plugin.SetFloatParameter( PT_SKIP, 0.0f );
		plugin.SetFloatParameter( PT_NIB, 0.5f );

		const int frames = static_cast< int >( rates[ i ] );
		for( int frame = 0; frame <= frames; ++frame )
			renderFrame( plugin, target, frame, rates[ i ] );

		totals[ i ] = totalDensity( readFloats( target ) );
		plugin.DeInitGL();

		std::printf( "  %5.0f fps %14.1f\n", rates[ i ], totals[ i ] );
	}

	const double lo = std::min( { totals[ 0 ], totals[ 1 ], totals[ 2 ] } );
	const double hi = std::max( { totals[ 0 ], totals[ 1 ], totals[ 2 ] } );
	const double spread = hi > 0.0 ? ( hi - lo ) / hi : 1.0;

	//One percent. Wider than --detail because the three runs do not chop the
	//path at the same places at all: at 24 fps a frame is five degrees of ring
	//and at 120 it is one, so the run boundaries -- and therefore the raster
	//sets near them -- differ everywhere rather than only at the cusps.
	const bool ok = spread < 0.01;
	std::printf( "\n  spread %.3f%%  %s\n", 100.0 * spread, ok ? "PASS" : "FAIL" );
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
// --beer
//---------------------------------------------------------------------------
//
// Driven through `Sheet` rather than through the plugin, because the claim is
// about two specific strokes crossing at a known point and the crank does not
// take requests. Two straight runs at right angles, in two different colours,
// crossing at the centre of the sheet.
//
// The identity being tested has no parameters in it at all:
//
//     shown( crossing ) * paper = shown( only A ) * shown( only B )
//
// It follows from nothing but the sheet holding a SUM of absorptions and the
// display pass being an exponential of it. Additive blending fails it, an alpha
// composite of each stroke fails it, and a max() against the history fails it.
int runBeer( const Target& target )
{
	std::printf( "beer -- two pens crossing multiply, they do not add\n\n" );

	Sheet sheet;
	if( !sheet.InitGL() )
		return 1;

	std::vector< Step > steps;
	std::vector< Run > runs;

	const int count = 64;
	auto addRun = [ & ]( float x0, float y0, float x1, float y1, const float colour[ 3 ] ) {
		Run run;
		run.first = static_cast< int >( steps.size() );
		run.count = count;
		run.colour[ 0 ] = colour[ 0 ];
		run.colour[ 1 ] = colour[ 1 ];
		run.colour[ 2 ] = colour[ 2 ];
		for( int i = 0; i < count; ++i )
		{
			const float t = static_cast< float >( i ) / static_cast< float >( count - 1 );
			Step s;
			s.x   = x0 + ( x1 - x0 ) * t;
			s.y   = y0 + ( y1 - y0 ) * t;
			s.ink = 1.0f;
			s.dt  = ( i + 1 < count ) ? ( 1.0f / static_cast< float >( count - 1 ) ) : 0.0f;
			steps.push_back( s );
		}
		runs.push_back( run );
	};

	const float red[ 3 ]  = { 0.80f, 0.25f, 0.20f };
	const float blue[ 3 ] = { 0.20f, 0.30f, 0.85f };

	Sheet::RenderParams params;
	//Chosen so that a single stroke transmits somewhere in the middle of the
	//range on every channel. It matters more than it looks: at flow 1 the peak
	//areal density is about 16, every channel transmits under 1e-6, and the
	//identity below is then satisfied by 0 == 0 * 0 -- a test that passes
	//against any renderer at all, including one that adds.
	params.flow      = 0.06f;
	params.nibSigma  = 0.02f;
	params.nibSpread = 0.0f;
	params.tooth     = 0.0f;
	params.paperGrain = 0.0f;
	params.fadeSeconds = 0.0f;
	params.scale     = 1.0f;
	params.paperColour[ 0 ] = params.paperColour[ 1 ] = params.paperColour[ 2 ] = 1.0f;
	params.gearLevel = 0.0f;
	params.opacity   = 1.0f;
	params.frameSeconds = 1.0f / 60.0f;

	const GLint viewport[ 4 ] = { 0, 0, target.width, target.height };
	Geometry geometry;// unused: the overlay is off

	auto renderOnce = [ & ]( bool withA, bool withB, float sampleRgb[ 3 ], float atX, float atY ) {
		steps.clear();
		runs.clear();
		if( withA )
			addRun( -0.6f, 0.0f, 0.6f, 0.0f, red );
		if( withB )
			addRun( 0.0f, -0.6f, 0.0f, 0.6f, blue );

		params.clearSheet = true;
		glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
		glViewport( 0, 0, target.width, target.height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		sheet.Render( steps, runs, geometry, 0.0, 0.0, params, target.fbo, viewport, 0, 1.0f, 1.0f );

		samplePixel( readFloats( target ), target.width, target.height, atX, atY, sampleRgb );
	};

	float both[ 3 ], onlyA[ 3 ], onlyB[ 3 ];
	renderOnce( true, true, both, 0.5f, 0.5f );
	renderOnce( true, false, onlyA, 0.5f, 0.5f );
	renderOnce( false, true, onlyB, 0.5f, 0.5f );

	sheet.DeInitGL();

	std::printf( "  channel   only A   only B   A then B   A*B (paper=1)   error\n" );

	int failures = 0;
	for( int c = 0; c < 3; ++c )
	{
		const double predicted = static_cast< double >( onlyA[ c ] ) * static_cast< double >( onlyB[ c ] );
		const double measured  = static_cast< double >( both[ c ] );
		const double error     = std::fabs( measured - predicted );

		//Absolute, not relative: the quantity is a transmission in 0..1, and a
		//relative tolerance on a channel that transmits 0.02 would be asserting
		//something far tighter than the float target can carry.
		const bool ok = error < 2.0e-3;
		if( !ok )
			++failures;

		std::printf( "  %-9s %8.5f %8.5f %10.5f %15.5f %8.2e %s\n",
		             c == 0 ? "red" : ( c == 1 ? "green" : "blue" ),
		             onlyA[ c ], onlyB[ c ], measured, predicted, error, ok ? "" : "FAIL" );
	}

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --liveness
//---------------------------------------------------------------------------
//
// It measures INKED AREA, not how much the picture changed, and the difference
// matters. A dead machine is not a still one: the pen is still going round,
// still laying ink down, and every pass darkens the line it is retracing -- so
// "how much did the frame change" reports a machine that has stopped drawing
// as very much alive, which is exactly what the first version of this test
// did. What a dead machine stops doing is reaching anywhere NEW. So the
// question is how much fresh sheet the pen found in the last five seconds,
// and re-darkening does not count.
int runLiveness( const Target& target )
{
	std::printf( "liveness -- the defaults keep drawing, and a stiff machine stops\n\n" );

	// Fraction of the sheet the pen has reached, measured against the same
	// machine with the flow shut off so the paper's own grain cancels.
	auto coverage = [ & ]( CogwheelPlugin& plugin, const std::vector< float >& blank ) {
		const std::vector< float > frame = readFloats( target );
		size_t marked = 0, n = 0;
		for( size_t p = 0; p < frame.size(); p += 4 )
		{
			const double luma = 0.299 * frame[ p ] + 0.587 * frame[ p + 1 ] + 0.114 * frame[ p + 2 ];
			const double bare = 0.299 * blank[ p ] + 0.587 * blank[ p + 1 ] + 0.114 * blank[ p + 2 ];
			if( std::fabs( luma - bare ) > 0.02 )
				++marked;
			++n;
		}
		( void )plugin;
		return n > 0 ? static_cast< double >( marked ) / static_cast< double >( n ) : 0.0;
	};

	auto configure = [ & ]( CogwheelPlugin& plugin, bool stiff, bool blank ) {
		if( stiff )
		{
			// One figure, the pen never lifted, a mesh that is exactly true and
			// no skipped teeth. This is the dead machine: after the figure
			// closes the pen retraces its own line for ever. 96/32 closes in
			// ONE turn, so it is dead within a second of starting.
			plugin.SetFloatParameter( PT_PRESET, 0.0f );
			plugin.SetFloatParameter( PT_LAYERS, 1.0f );
			plugin.SetFloatParameter( PT_CHANGE, static_cast< float >( Change::Nothing ) );
			plugin.SetFloatParameter( PT_CREEP, 0.0f );
			plugin.SetFloatParameter( PT_SKIP, 0.0f );
			plugin.SetFloatParameter( PT_FADE, 0.0f );
			plugin.SetFloatParameter( PT_WHEEL, 32.0f );
		}
		if( blank )
			plugin.SetFloatParameter( PT_FLOW, 0.0f );
	};

	auto growth = [ & ]( bool stiff, double& early, double& late ) {
		std::vector< float > blank;
		{
			CogwheelPlugin plugin( false );
			if( !startPlugin( plugin, target ) )
				return false;
			configure( plugin, stiff, true );
			renderFrame( plugin, target, 0 );
			blank = readFloats( target );
			plugin.DeInitGL();
		}

		CogwheelPlugin plugin( false );
		if( !startPlugin( plugin, target ) )
			return false;
		configure( plugin, stiff, false );

		//Five seconds to settle, then two five-second windows. The stiff
		//machine has closed its figure many times over before the first
		//measurement is taken.
		for( int frame = 0; frame <= 600; ++frame )
			renderFrame( plugin, target, frame );
		early = coverage( plugin, blank );

		for( int frame = 601; frame <= 900; ++frame )
			renderFrame( plugin, target, frame );
		late = coverage( plugin, blank );

		plugin.DeInitGL();
		return true;
	};

	double aliveEarly = 0.0, aliveLate = 0.0, deadEarly = 0.0, deadLate = 0.0;
	if( !growth( false, aliveEarly, aliveLate ) || !growth( true, deadEarly, deadLate ) )
		return 1;

	const double aliveGrowth = aliveLate - aliveEarly;
	const double deadGrowth   = deadLate - deadEarly;

	std::printf( "                     covered at 10s   at 15s    fresh sheet\n" );
	std::printf( "  defaults           %11.3f%% %8.3f%% %12.4f%%\n",
	             100.0 * aliveEarly, 100.0 * aliveLate, 100.0 * aliveGrowth );
	std::printf( "  creep 0, 1 layer   %11.3f%% %8.3f%% %12.4f%%\n\n",
	             100.0 * deadEarly, 100.0 * deadLate, 100.0 * deadGrowth );

	//A twentieth of a percent of the sheet in five seconds. At 320x180 that is
	//about thirty pixels -- a fraction of one stroke -- so it is a low bar
	//deliberately: the claim is "still drawing", not "drawing fast".
	const bool aliveOk = aliveGrowth > 0.0005;

	//The dead machine is allowed a tenth of the live one's growth, and not
	//zero: a line that is being retraced does slowly darken, and a pixel at the
	//very edge of the nib's profile can cross the threshold long after the pen
	//stopped finding new sheet.
	const bool deadOk = deadGrowth < aliveGrowth * 0.1;

	if( !aliveOk )
		std::printf( "  FAIL: the default machine has stopped finding fresh sheet.\n"
		             "        Creep, Layers or Skip has been turned off in the defaults --\n"
		             "        see machine/Crank.h for why one of them has to be on.\n" );
	if( !deadOk )
		std::printf( "  FAIL: the stiff machine did not stop, so this test is measuring\n"
		             "        something other than what it claims to.\n" );

	const bool ok = aliveOk && deadOk;
	std::printf( "  %s\n", ok ? "PASS" : "FAIL" );
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
// --presets
//---------------------------------------------------------------------------
//
// "Covered" is measured against the SAME preset rendered with the flow shut
// off, not against a corner of the sheet. That matters for any preset with a
// visible grain in it: Chalkboard's paper differs from a flat reference by
// more than the threshold at every pixel, so a corner-referenced measurement
// reported it as 96% covered and called a perfectly good drawing a failure.
// Differencing against the blank sheet asks the question that was meant --
// where did the pen actually reach -- and is exact, because both renders are
// deterministic and the grain is a function of position alone.
int runPresets( const Target& target )
{
	std::printf( "presets -- every one draws something, with structure in it\n\n" );
	std::printf( "  preset              covered   contrast\n" );

	int failures = 0;
	for( int i = 1; i <= presets::kCount; ++i )
	{
		std::vector< float > drawn, blank;

		for( int pass = 0; pass < 2; ++pass )
		{
			CogwheelPlugin plugin( false );
			if( !startPlugin( plugin, target ) )
				return 1;

			plugin.SetFloatParameter( PT_PRESET, static_cast< float >( i ) );
			if( pass == 1 )
				plugin.SetFloatParameter( PT_FLOW, 0.0f );//the same sheet, undrawn on

			//Chosen against the SLOWEST preset, not picked round. Show the
			//Gears is cranked at a third of a turn a second on purpose, so it
			//is the one that decides this number: at 400 frames it has been
			//round the ring more than twice and measures 2.0% covered against
			//a 0.2% floor, with a contrast of 0.073 against a floor of 0.02.
			//Every other preset has more margin than that. 180 frames was the
			//first attempt and it failed here, correctly.
			//
			//It is also the most expensive number in the harness -- eight
			//presets, twice each -- so it is worth knowing what it buys rather
			//than rounding it up for comfort. On a CI runner with no GPU this
			//check alone was two fifths of the whole suite.
			for( int frame = 0; frame < 400; ++frame )
				renderFrame( plugin, target, frame );

			( pass == 0 ? drawn : blank ) = readFloats( target );
			plugin.DeInitGL();
		}

		double sum = 0.0, sumSq = 0.0;
		size_t marked = 0, n = 0;
		for( size_t p = 0; p < drawn.size(); p += 4 )
		{
			const double luma = 0.299 * drawn[ p ] + 0.587 * drawn[ p + 1 ] + 0.114 * drawn[ p + 2 ];
			const double bare = 0.299 * blank[ p ] + 0.587 * blank[ p + 1 ] + 0.114 * blank[ p + 2 ];
			sum += luma;
			sumSq += luma * luma;
			if( std::fabs( luma - bare ) > 0.02 )
				++marked;
			++n;
		}

		const double covered  = n > 0 ? static_cast< double >( marked ) / static_cast< double >( n ) : 0.0;
		const double mean     = n > 0 ? sum / static_cast< double >( n ) : 0.0;
		const double variance = n > 0 ? std::max( 0.0, sumSq / static_cast< double >( n ) - mean * mean ) : 0.0;
		const double contrast = std::sqrt( variance );

		// Two different questions. "Covered" catches a preset that draws
		// nothing, and one whose figure has gone off the edge of the frame.
		// "Contrast" catches a preset that has filled the sheet solid -- which
		// coverage on its own would report as a triumph.
		//A fifth of a percent of the sheet. Low because it is measuring INKED
		//PIXELS at 320x180, where a single stroke of a fine nib is about a
		//thousandth of the frame -- a drawing of a dozen strokes is a couple of
		//percent and a drawing of none is zero. The upper bound is the one that
		//catches a sheet filled solid.
		const bool ok = covered > 0.002 && covered < 0.95 && contrast > 0.02;
		if( !ok )
			++failures;

		std::printf( "  %-20s %6.1f%% %10.4f  %s\n",
		             presets::kPresets[ i - 1 ].name, 100.0 * covered, contrast,
		             ok ? "ok" : "FAIL" );
	}

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --defaults
//---------------------------------------------------------------------------
int runDefaults()
{
	std::printf( "defaults -- the constructor's values ARE preset 1\n\n" );

	CogwheelPlugin plugin( false );

	int count             = 0;
	const unsigned int* ids = CogwheelPlugin::PresetParamIDsForTest( count );

	int failures = 0;
	for( int j = 0; j < count; ++j )
	{
		const float actual   = plugin.GetFloatParameter( ids[ j ] );
		const float expected = presets::kPresets[ 0 ].v[ j ];
		if( std::fabs( actual - expected ) > 1e-6f )
		{
			std::printf( "  parameter %-3u  is %.6f, preset 1 says %.6f  FAIL\n",
			             ids[ j ], actual, expected );
			++failures;
		}
	}

	if( failures == 0 )
		std::printf( "  all %d covered parameters agree\n", count );
	else
		std::printf( "\n  Retune one, retune the other. See the note in Presets.h.\n" );

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --hosts
//---------------------------------------------------------------------------
//
// The test shape the fleet asks for after vertigo #2. A preset must survive all
// three things a host can do with the value events the plugin raises:
//
//   honours     re-reads every parameter and pushes the new value back
//   ignores     carries on pushing the values it still believes in -- which is
//               what Resolume does, and what broke seven plugins
//   quantises   honours the event but hands back a rounded copy
//
// The "ignores" column is the one that fails against a naive implementation,
// and it fails for every preset whose values differ from the defaults.
int runHosts()
{
	std::printf( "hosts -- presets survive every host behaviour\n\n" );
	std::printf( "  preset                honours   ignores   quantises\n" );

	int count               = 0;
	const unsigned int* ids = CogwheelPlugin::PresetParamIDsForTest( count );

	enum Behaviour
	{
		Honours,
		Ignores,
		Quantises,
		BehaviourCount
	};

	int failures = 0;
	for( int i = 1; i <= presets::kCount; ++i )
	{
		bool result[ BehaviourCount ] = {};

		for( int b = 0; b < BehaviourCount; ++b )
		{
			CogwheelPlugin plugin( false );

			// What the host believes before the preset is chosen.
			std::vector< float > believed( static_cast< size_t >( count ) );
			for( int j = 0; j < count; ++j )
				believed[ j ] = plugin.GetFloatParameter( ids[ j ] );

			plugin.SetFloatParameter( PT_PRESET, static_cast< float >( i ) );

			// The host's next round of parameter traffic. Twice, because a host
			// that pushes every frame pushes more than once and the bug this
			// guards against only needed one.
			for( int pass = 0; pass < 2; ++pass )
			{
				for( int j = 0; j < count; ++j )
				{
					float push = 0.0f;
					switch( b )
					{
						case Honours:   push = plugin.GetFloatParameter( ids[ j ] ); break;
						case Ignores:   push = believed[ j ]; break;
						default:
						{
							const float value = plugin.GetFloatParameter( ids[ j ] );
							push = std::round( value * 1000.0f ) / 1000.0f;
							break;
						}
					}
					plugin.SetFloatParameter( ids[ j ], push );
				}
			}

			bool ok = std::lround( plugin.GetFloatParameter( PT_PRESET ) ) == i;
			for( int j = 0; j < count && ok; ++j )
			{
				const float expected = presets::kPresets[ i - 1 ].v[ j ];
				//The same quantisation allowance the plugin itself uses.
				if( std::fabs( plugin.GetFloatParameter( ids[ j ] ) - expected ) > 1.5e-3f )
					ok = false;
			}
			result[ b ] = ok;
			if( !ok )
				++failures;
		}

		std::printf( "  %-20s %8s %9s %11s\n", presets::kPresets[ i - 1 ].name,
		             result[ Honours ] ? "ok" : "FAIL",
		             result[ Ignores ] ? "ok" : "FAIL",
		             result[ Quantises ] ? "ok" : "FAIL" );
	}

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --scale
//---------------------------------------------------------------------------
//
// The same preset must be the same DRAWING at every raster. Everything the
// renderer measures is a fraction of the sheet -- the nib, the grain, the
// framing -- so a figure at 320x180 and the same figure at 1280x720 should
// differ only in how finely each is sampled.
//
// The escapement lesson being guarded: a length expressed in PIXELS looks
// right at whatever resolution it was tuned at and is wrong everywhere else,
// and the symptom is a preset with a different number of visible features on
// somebody else's machine. Escapement rendered eight cells across the frame at
// 320x180 and thirty-six at 1280x720 from one preset.
//
// Two things about HOW it is measured, both learned by getting them wrong:
//
// - It compares DENSITY, box-averaged into 80x45 cells, not what the display
//   pass shows. `shown` is an exponential of density, so averaging it does not
//   commute with averaging the ink -- and a stroke narrower than a pixel is
//   the case where that bites hardest.
// - It forces a nib nearly two pixels wide even at 320x180. A stroke thinner
//   than a pixel is not sampled, it is point-sampled, and the ink that falls
//   between two pixel centres is simply lost. That is a property of rasters
//   rather than of this plugin, and a test that reported it would fail on
//   correct code for ever.
int runScale()
{
	std::printf( "scale -- the same preset is the same drawing at every raster\n\n" );
	std::printf( "  preset                 mean density        difference\n" );

	constexpr int kCols = 80;
	constexpr int kRows = 45;

	int failures = 0;
	for( int i = 1; i <= presets::kCount; ++i )
	{
		std::vector< double > cells[ 2 ];
		double means[ 2 ] = {};
		const int sizes[ 2 ][ 2 ] = { { 320, 180 }, { 1280, 720 } };

		for( int s = 0; s < 2; ++s )
		{
			Target target = makeTarget( sizes[ s ][ 0 ], sizes[ s ][ 1 ] );

			CogwheelPlugin plugin( false );
			if( !startPlugin( plugin, target ) )
			{
				releaseTarget( target );
				return 1;
			}

			plugin.SetFloatParameter( PT_PRESET, static_cast< float >( i ) );
			setMeasurementLook( plugin );
			plugin.SetFloatParameter( PT_NIB, 0.85f );

			for( int frame = 0; frame < 120; ++frame )
				renderFrame( plugin, target, frame );

			cells[ s ] = boxDown( densityImage( readFloats( target ) ),
			                      target.width, target.height, kCols, kRows );
			plugin.DeInitGL();
			releaseTarget( target );

			double sum = 0.0;
			for( double v : cells[ s ] )
				sum += v;
			means[ s ] = sum / static_cast< double >( cells[ s ].size() );
		}

		double error = 0.0;
		for( size_t c = 0; c < cells[ 0 ].size(); ++c )
			error += std::fabs( cells[ 0 ][ c ] - cells[ 1 ][ c ] );
		error /= static_cast< double >( cells[ 0 ].size() );

		const double reference = std::max( 1.0e-6, 0.5 * ( means[ 0 ] + means[ 1 ] ) );
		const double relative  = error / reference;

		// A tenth. What is left after the density fix and the fat nib is the
		// residual sampling difference along the edge of every stroke, and it
		// scales with how much edge a preset has -- Dense Web has a great deal.
		// A figure with a different NUMBER of strokes in it moves this by well
		// over one, not by a tenth.
		const bool ok = relative < 0.10;
		if( !ok )
			++failures;

		std::printf( "  %-20s %8.4f %9.4f %11.3f  %s\n",
		             presets::kPresets[ i - 1 ].name, means[ 0 ], means[ 1 ], relative,
		             ok ? "ok" : "FAIL" );
	}

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --guard
//---------------------------------------------------------------------------
//
// The sheet accumulates for the whole life of the drawing, so ONE NaN in it is
// permanent: NaN times a fade is NaN, and NaN plus a stroke is NaN. Every
// hostile machine the parameter ranges allow is driven here, and the sheet is
// read back and searched.
int runGuard( const Target& target )
{
	std::printf( "guard -- a hostile machine leaves no NaN on the sheet\n\n" );

	struct Case
	{
		const char* name;
		unsigned int id;
		float value;
	};
	const Case cases[] = {
		{ "wheel bigger than the ring", PT_WHEEL, 240.0f },
		{ "wheel the same as the ring", PT_WHEEL, 96.0f },
		{ "the smallest wheel", PT_WHEEL, 3.0f },
		{ "the smallest ring", PT_RING, 12.0f },
		{ "the crank stopped", PT_RATE, 0.0f },
		{ "the crank flat out", PT_RATE, 1.0f },
		{ "the pen on the axle", PT_PEN, 0.0f },
		{ "the pen on the rim", PT_PEN, 1.0f },
		{ "the nib shut", PT_NIB, 0.0f },
		{ "the nib wide open", PT_NIB, 1.0f },
		{ "no flow", PT_FLOW, 0.0f },
		{ "flow flat out", PT_FLOW, 1.0f },
		{ "the sheet shrunk to nothing", PT_ZOOM, 0.0f },
		{ "the sheet blown up", PT_ZOOM, 1.0f },
		{ "a mesh a tooth out every turn", PT_CREEP, 1.0f },
		{ "a skip every turn", PT_SKIP, 1.0f },
		{ "black ink", PT_INK_R, 0.0f },
	};

	int failures = 0;
	for( const Case& c : cases )
	{
		CogwheelPlugin plugin( false );
		if( !startPlugin( plugin, target ) )
			return 1;

		plugin.SetFloatParameter( PT_PRESET, 0.0f );
		plugin.SetFloatParameter( c.id, c.value );

		for( int frame = 0; frame < 60; ++frame )
			renderFrame( plugin, target, frame );

		const std::vector< float > frame = readFloats( target );
		plugin.DeInitGL();

		size_t bad = 0;
		for( float v : frame )
			if( !( v > -1.0e30f && v < 1.0e30f ) )
				++bad;

		const bool ok = bad == 0;
		if( !ok )
			++failures;

		std::printf( "  %-32s %s\n", c.name, ok ? "clean" : "FAIL -- NaN or inf on the sheet" );
	}

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --list, --names
//---------------------------------------------------------------------------
int runList( bool namesOnly )
{
	CogwheelPlugin plugin( false );

	if( namesOnly )
	{
		//FFGL truncates a parameter name at 16 characters, and the truncation
		//happens in the host, silently. Anything over is a control whose label
		//an operator cannot read.
		std::printf( "names longer than FFGL's 16 characters:\n\n" );
		int over = 0;
		for( unsigned int id = 0; id < PT_COUNT; ++id )
		{
			const char* name = plugin.GetParamName( id );
			if( name == nullptr )
				continue;
			const size_t length = std::strlen( name );
			if( length > 16 )
			{
				std::printf( "  %-3u  %-28s %zu\n", id, name, length );
				++over;
			}
		}
		std::printf( "\n  %d over the limit\n", over );
		return over == 0 ? 0 : 1;
	}

	//The format is the fleet's, and `tools/sweep.py` parses it: id, name, kind,
	//value, range, meaning. The kind and the range are read out of the SDK
	//rather than out of a table here, so a parameter that was declared as
	//something other than what it looks like cannot hide.
	std::printf( "%-4s %-22s %-9s %10s   %-16s %s\n",
	             "id", "name", "kind", "value", "range", "means" );
	for( unsigned int id = 0; id < PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );

		//The About block is stopped here rather than swept or displayed. The
		//text line has no value and the four buttons OPEN A WEB BROWSER when
		//their value is set -- one tab per press, which a sweep would do
		//eight times.
		if( id >= PT_ABOUT_TEXT )
		{
			std::printf( "%-4u %-22s %-9s %10s   %-16s %s\n", id, name ? name : "",
			             "about", "-", "-", "the Stoatworks About block; not swept" );
			continue;
		}

		const char* display = plugin.GetParameterDisplay( id );

		const char* kind = "standard";
		switch( plugin.GetParamType( id ) )
		{
			case FF_TYPE_BOOLEAN: kind = "boolean"; break;
			case FF_TYPE_EVENT:   kind = "event"; break;
			case FF_TYPE_INTEGER: kind = "integer"; break;
			case FF_TYPE_OPTION:  kind = "option"; break;
			case FF_TYPE_TEXT:    kind = "text"; break;
			case FF_TYPE_RED:
			case FF_TYPE_GREEN:
			case FF_TYPE_BLUE:    kind = "colour"; break;
			default: break;
		}

		RangeStruct range = plugin.GetParamRange( id );
		//An option's range is not the 0..1 the SDK reports: the parameter holds
		//an element VALUE. Printing 0..1 would make tools/sweep.py sweep every
		//dropdown between its first two entries and report the rest as
		//unreachable -- which for Pens, On Closing and Print is most of them.
		if( plugin.GetParamType( id ) == FF_TYPE_OPTION )
		{
			range.min = 0.0f;
			range.max = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( id ) ) ) - 1.0f;
		}
		char rangeText[ 32 ] = {};
		std::snprintf( rangeText, sizeof( rangeText ), "[%g .. %g]", range.min, range.max );

		std::printf( "%-4u %-22s %-9s %10.4f   %-16s %s\n", id, name ? name : "", kind,
		             plugin.GetFloatParameter( id ), rangeText, display ? display : "" );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --out, --contact
//---------------------------------------------------------------------------
GLuint makeTestClip( int width, int height )
{
	//Four quadrants of strong flat colour plus a grey ramp. Chosen for the
	//effect build's Ink from Clip: the honest claim in the plugin's description
	//is that it is excellent over large areas of strong colour and mud over
	//anything busy, and this is the first half of that.
	std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = static_cast< float >( x ) / static_cast< float >( width );
			const float v = static_cast< float >( y ) / static_cast< float >( height );
			float* p      = pixels.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			p[ 0 ] = u < 0.5f ? ( v < 0.5f ? 0.85f : 0.15f ) : ( v < 0.5f ? 0.15f : 0.75f );
			p[ 1 ] = u < 0.5f ? ( v < 0.5f ? 0.20f : 0.70f ) : ( v < 0.5f ? 0.55f : 0.75f );
			p[ 2 ] = u < 0.5f ? ( v < 0.5f ? 0.20f : 0.25f ) : ( v < 0.5f ? 0.80f : 0.20f );
			p[ 3 ] = 1.0f;
		}
	}

	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, pixels.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

int runOut( const std::string& path, int width, int height, int frames, int preset, bool effect,
            const std::vector< std::pair< std::string, float > >& sets )
{
	Target target = makeTarget( width, height );

	CogwheelPlugin plugin( effect );
	if( !startPlugin( plugin, target ) )
	{
		releaseTarget( target );
		return 1;
	}

	GLuint clip = effect ? makeTestClip( width, height ) : 0;

	if( preset >= 0 )
		plugin.SetFloatParameter( PT_PRESET, static_cast< float >( preset ) );
	if( applySets( plugin, sets ) != 0 )
	{
		releaseTarget( target );
		return 1;
	}

	for( int frame = 0; frame < std::max( 1, frames ); ++frame )
		renderFrame( plugin, target, frame, 60.0, clip );

	const std::vector< float > pixels = readFloats( target );
	plugin.DeInitGL();
	if( clip != 0 )
		glDeleteTextures( 1, &clip );

	const bool written = writePng( path, width, height, toBytes( pixels, width, height ) );
	releaseTarget( target );

	if( !written )
	{
		std::fprintf( stderr, "cgtest: could not write %s\n", path.c_str() );
		return 1;
	}
	std::printf( "%s  %dx%d, %d frames\n", path.c_str(), width, height, std::max( 1, frames ) );
	return 0;
}

int runContact( const std::string& path, int frames )
{
	//Four across, however many rows that needs. Each cell is 480x270, so the
	//sheet is 1920 wide and the aspect of every cell is the 16:9 the plugin is
	//nearly always asked for.
	constexpr int kCellWidth  = 480;
	constexpr int kCellHeight = 270;
	constexpr int kColumns    = 4;

	const int rows   = ( presets::kCount + kColumns - 1 ) / kColumns;
	const int width  = kCellWidth * kColumns;
	const int height = kCellHeight * rows;

	std::vector< unsigned char > sheetPixels( static_cast< size_t >( width ) * height * 4, 0 );

	Target target = makeTarget( kCellWidth, kCellHeight );

	for( int i = 1; i <= presets::kCount; ++i )
	{
		CogwheelPlugin plugin( false );
		if( !startPlugin( plugin, target ) )
		{
			releaseTarget( target );
			return 1;
		}

		plugin.SetFloatParameter( PT_PRESET, static_cast< float >( i ) );
		for( int frame = 0; frame < std::max( 1, frames ); ++frame )
			renderFrame( plugin, target, frame );

		const std::vector< unsigned char > cell =
			toBytes( readFloats( target ), kCellWidth, kCellHeight );
		plugin.DeInitGL();

		const int column = ( i - 1 ) % kColumns;
		const int row    = ( i - 1 ) / kColumns;
		for( int y = 0; y < kCellHeight; ++y )
		{
			std::memcpy( sheetPixels.data()
			                 + ( ( static_cast< size_t >( row * kCellHeight + y ) * width )
			                     + static_cast< size_t >( column * kCellWidth ) ) * 4,
			             cell.data() + static_cast< size_t >( y ) * kCellWidth * 4,
			             static_cast< size_t >( kCellWidth ) * 4 );
		}
	}

	releaseTarget( target );

	if( !writePng( path, width, height, sheetPixels ) )
	{
		std::fprintf( stderr, "cgtest: could not write %s\n", path.c_str() );
		return 1;
	}
	std::printf( "%s  %d presets, %dx%d\n", path.c_str(), presets::kCount, width, height );
	return 0;
}

//---------------------------------------------------------------------------
// --sequence
//---------------------------------------------------------------------------
struct Cue
{
	double from = 0.0;
	double to   = 0.0;
	std::string name;
	float first  = 0.0f;
	float second = 0.0f;
	bool ramp    = false;
};

/// `T Name=V` sets at a time; `T1..T2 Name=V1..V2` ramps between two. Comments
/// start at `#`. Every value is the 0..1 the host sees, because that is the only
/// kind there is -- see Controls.h.
bool parseCues( const std::string& path, std::vector< Cue >& cues )
{
	std::FILE* file = std::fopen( path.c_str(), "rb" );
	if( file == nullptr )
	{
		std::fprintf( stderr, "cannot open cue sheet %s\n", path.c_str() );
		return false;
	}

	char line[ 1024 ];
	int number = 0;
	while( std::fgets( line, sizeof( line ), file ) != nullptr )
	{
		++number;
		std::string text = line;

		const size_t hash = text.find( '#' );
		if( hash != std::string::npos )
			text = text.substr( 0, hash );

		const size_t firstReal = text.find_first_not_of( " \t\r\n" );
		if( firstReal == std::string::npos )
			continue;
		text = text.substr( firstReal );

		const size_t split = text.find_first_of( " \t" );
		if( split == std::string::npos )
			continue;

		const std::string when = text.substr( 0, split );
		std::string assignment = text.substr( split );

		const size_t assignStart = assignment.find_first_not_of( " \t" );
		if( assignStart == std::string::npos )
			continue;
		assignment = assignment.substr( assignStart );
		while( !assignment.empty() && std::strchr( "\r\n \t", assignment.back() ) != nullptr )
			assignment.pop_back();

		Cue cue;
		const size_t timeRange = when.find( ".." );
		if( timeRange != std::string::npos )
		{
			cue.from = std::strtod( when.substr( 0, timeRange ).c_str(), nullptr );
			cue.to   = std::strtod( when.substr( timeRange + 2 ).c_str(), nullptr );
			cue.ramp = true;
		}
		else
		{
			cue.from = cue.to = std::strtod( when.c_str(), nullptr );
		}

		const size_t equals = assignment.find( '=' );
		if( equals == std::string::npos )
		{
			std::fprintf( stderr, "%s:%d: expected Name=value\n", path.c_str(), number );
			std::fclose( file );
			return false;
		}

		cue.name                = assignment.substr( 0, equals );
		const std::string value = assignment.substr( equals + 1 );

		const size_t valueRange = value.find( ".." );
		if( cue.ramp && valueRange != std::string::npos )
		{
			cue.first  = std::strtof( value.substr( 0, valueRange ).c_str(), nullptr );
			cue.second = std::strtof( value.substr( valueRange + 2 ).c_str(), nullptr );
		}
		else
		{
			cue.first = cue.second = std::strtof( value.c_str(), nullptr );
			cue.ramp  = false;
		}

		cues.push_back( cue );
	}

	std::fclose( file );
	return true;
}

/**
    Render a cue-sheet driven frame sequence.

    **One plugin instance for the whole piece**, and for cogwheel that is not a
    detail -- it is the argument. The sheet is the drawing, so a change of wheel
    or hole part way through draws the next figure ON TOP of what is already
    there, exactly as it does when a person at the table swaps a wheel. Rendering
    each section into a fresh instance would produce a sequence of unrelated
    pictures and quietly contradict the thing the video is about.

    The clock is driven from the frame counter, so the render is deterministic
    and independent of how fast the GPU happens to be.
*/
int runSequence( const std::string& directory, const std::string& cuePath,
                 int width, int height, double seconds, double fps, bool effect )
{
	std::vector< Cue > cues;
	if( !cuePath.empty() && !parseCues( cuePath, cues ) )
		return 1;

	Target target = makeTarget( width, height );

	CogwheelPlugin plugin( effect );
	if( !startPlugin( plugin, target ) )
	{
		releaseTarget( target );
		return 1;
	}

	// Every cue is checked against the real parameter list before a single frame
	// is rendered. A typo in a name would otherwise be a cue that silently never
	// fires, and the only symptom would be a video subtly less interesting than
	// the sheet says it is.
	const std::map< std::string, unsigned int > byName = parameterIndex( plugin );
	for( const Cue& cue : cues )
	{
		if( byName.find( cue.name ) == byName.end() )
		{
			std::fprintf( stderr, "cue names '%s', which is not a parameter\n", cue.name.c_str() );
			releaseTarget( target );
			return 1;
		}
	}

	const GLuint clip = effect ? makeTestClip( width, height ) : 0;
	const int frames  = static_cast< int >( seconds * fps + 0.5 );

	//---------------------------------------------------------------------
	// Which cues address an EVENT parameter, and whether each has fired.
	//
	// ☠️ Every other cue is re-applied on every frame from its time onwards --
	// that is what makes "a later cue on the same parameter wins" true, and it
	// is right for a value. It is catastrophic for an event. `New Sheet` is a
	// rising edge, so a pair of cues holding it at 1 and then 0 re-fires the
	// edge on EVERY FRAME: the sheet is wiped sixty times a second and the
	// drawing never accumulates at all. The first render of this piece came
	// back with four of its seven sections showing a single short arc, and the
	// cue sheet was innocent.
	//
	// So an event cue fires exactly once, on the first frame at or after its
	// time, and is then inert.
	//---------------------------------------------------------------------
	std::vector< bool > isEvent( cues.size(), false );
	std::vector< bool > fired( cues.size(), false );
	for( size_t i = 0; i < cues.size(); ++i )
		isEvent[ i ] = plugin.GetParamType( byName.at( cues[ i ].name ) ) == FF_TYPE_EVENT;

	for( int frame = 0; frame < frames; ++frame )
	{
		const double now = static_cast< double >( frame ) / fps;

		// Cues are applied in file order every frame rather than tracked as
		// state, so a later cue on the same parameter simply wins -- which is
		// what reading the sheet top to bottom leads you to expect.
		for( size_t i = 0; i < cues.size(); ++i )
		{
			const Cue& cue = cues[ i ];
			if( now < cue.from )
				continue;

			if( isEvent[ i ] )
			{
				if( fired[ i ] )
					continue;
				fired[ i ] = true;
				plugin.SetFloatParameter( byName.at( cue.name ), cue.second );
				continue;
			}

			float value = cue.second;
			if( cue.ramp && now < cue.to && cue.to > cue.from )
			{
				const double t = ( now - cue.from ) / ( cue.to - cue.from );
				// Smoothstep rather than linear: a parameter that starts and
				// stops abruptly reads as a jump cut even when every value in
				// between is right.
				const double eased = t * t * ( 3.0 - 2.0 * t );
				value = static_cast< float >( cue.first + ( cue.second - cue.first ) * eased );
			}

			plugin.SetFloatParameter( byName.at( cue.name ), value );
		}

		if( !renderFrame( plugin, target, frame, fps, clip ) )
		{
			std::fprintf( stderr, "render failed at frame %d\n", frame );
			releaseTarget( target );
			return 1;
		}

		char path[ 1024 ];
		std::snprintf( path, sizeof( path ), "%s/frame%05d.png", directory.c_str(), frame );
		if( !writePng( path, width, height, toBytes( readFloats( target ), width, height ) ) )
		{
			std::fprintf( stderr, "could not write %s\n", path );
			releaseTarget( target );
			return 1;
		}

		if( ( frame + 1 ) % 60 == 0 )
			std::printf( "  %d / %d frames\n", frame + 1, frames );
	}

	plugin.DeInitGL();
	if( clip != 0 )
		glDeleteTextures( 1, &clip );
	releaseTarget( target );

	std::printf( "%d frames -> %s\n", frames, directory.c_str() );
	return 0;
}

void usage()
{
	std::printf(
		"cgtest -- the cogwheel offline harness\n\n"
		"  --closure   the gear arithmetic, and the pen coming home\n"
		"  --detail    total ink is the same at Draft, Normal and Fine\n"
		"  --rate      one second of cranking at 24, 60 and 120 fps\n"
		"  --beer      two pens crossing multiply, they do not add\n"
		"  --liveness  the defaults keep drawing; a stiff machine stops\n"
		"  --presets   every preset draws something with structure in it\n"
		"  --defaults  the constructor's defaults ARE preset 1\n"
		"  --hosts     presets survive all three host behaviours\n"
		"  --scale     the same preset is the same drawing at every raster\n"
		"  --guard     a hostile machine leaves no NaN on the sheet\n"
		"  --all       every one of the above\n\n"
		"  --out PATH [--size WxH] [--frames N] [--preset N] [--effect]\n"
		"  --contact PATH [--frames N]\n"
		"  --sequence DIR --script FILE [--size WxH] [--seconds N] [--fps N] [--effect]\n"
		"  --list      every parameter and what it currently means\n"
		"  --names     parameter names over FFGL's 16-character limit\n"
		"  --set ID=V  or --set \"Name=V\", repeatable\n" );
}
} // namespace

int main( int argc, char** argv )
{
	if( argc < 2 )
	{
		usage();
		return 2;
	}

	std::string outPath;
	std::string contactPath;
	std::string sequenceDir;
	std::string scriptPath;
	double seconds = 60.0;
	double fps     = 30.0;
	int width   = 1280;
	int height  = 720;
	int frames  = 120;
	int preset  = -1;
	bool effect = false;
	std::vector< std::pair< std::string, float > > sets;
	std::vector< std::string > checks;
	bool list = false, names = false;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		auto next             = [ & ]() -> std::string { return ( i + 1 < argc ) ? argv[ ++i ] : std::string(); };

		if( arg == "--out" )
			outPath = next();
		else if( arg == "--contact" )
			contactPath = next();
		else if( arg == "--sequence" )
			sequenceDir = next();
		else if( arg == "--script" )
			scriptPath = next();
		else if( arg == "--seconds" )
			seconds = std::atof( next().c_str() );
		else if( arg == "--fps" )
			fps = std::atof( next().c_str() );
		else if( arg == "--size" )
		{
			const std::string size = next();
			const size_t x         = size.find( 'x' );
			if( x != std::string::npos )
			{
				width  = std::atoi( size.substr( 0, x ).c_str() );
				height = std::atoi( size.substr( x + 1 ).c_str() );
			}
		}
		else if( arg == "--frames" )
			frames = std::atoi( next().c_str() );
		else if( arg == "--preset" )
			preset = std::atoi( next().c_str() );
		else if( arg == "--effect" )
			effect = true;
		else if( arg == "--list" )
			list = true;
		else if( arg == "--names" )
			names = true;
		else if( arg == "--set" )
		{
			const std::string pair = next();
			const size_t equals    = pair.find( '=' );
			if( equals != std::string::npos )
				sets.emplace_back( pair.substr( 0, equals ),
				                   static_cast< float >( std::atof( pair.substr( equals + 1 ).c_str() ) ) );
		}
		else if( arg.rfind( "--", 0 ) == 0 )
			checks.push_back( arg.substr( 2 ) );
		else
		{
			usage();
			return 2;
		}
	}

	// --closure, --defaults and --hosts need no GL at all, so they are answered
	// before a context is created: on a machine with no GPU available they are
	// still the tests that can run.
	if( checks.size() == 1 && ( checks[ 0 ] == "closure" || checks[ 0 ] == "defaults" || checks[ 0 ] == "hosts" ) )
	{
		if( checks[ 0 ] == "closure" )
			return runClosure();
		if( checks[ 0 ] == "defaults" )
			return runDefaults();
		return runHosts();
	}

	if( list || names )
		return runList( names );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "cgtest: no OpenGL 4.1 core context available\n" );
		return 1;
	}

	int result = 0;

	if( !sequenceDir.empty() )
		result |= runSequence( sequenceDir, scriptPath, width, height, seconds, fps, effect );

	if( !outPath.empty() )
		result |= runOut( outPath, width, height, frames, preset, effect, sets );

	if( !contactPath.empty() )
		result |= runContact( contactPath, frames );

	if( !checks.empty() )
	{
		const bool all = std::find( checks.begin(), checks.end(), "all" ) != checks.end();
		auto wanted    = [ & ]( const char* name ) {
			return all || std::find( checks.begin(), checks.end(), name ) != checks.end();
		};

		//320x180. Small on purpose: every check here is a sum or a ratio over
		//the whole frame, none of them looks at fine detail, and at this size a
		//three-hundred-frame run finishes in about a second. --scale makes its
		//own targets, because its whole question is the raster.
		Target target = makeTarget( 320, 180 );

		int failed = 0, ran = 0;
		auto run = [ & ]( const char* name, int ( *fn )( const Target& ) ) {
			if( !wanted( name ) )
				return;
			++ran;
			std::printf( "\n" );
			failed += fn( target );
		};

		if( wanted( "closure" ) ) { ++ran; std::printf( "\n" ); failed += runClosure(); }
		run( "detail", runDetail );
		run( "rate", runRate );
		run( "beer", runBeer );
		run( "liveness", runLiveness );
		run( "presets", runPresets );
		if( wanted( "defaults" ) ) { ++ran; std::printf( "\n" ); failed += runDefaults(); }
		if( wanted( "hosts" ) )    { ++ran; std::printf( "\n" ); failed += runHosts(); }
		if( wanted( "scale" ) )    { ++ran; std::printf( "\n" ); failed += runScale(); }
		run( "guard", runGuard );

		releaseTarget( target );

		if( ran == 0 )
		{
			usage();
			result = 2;
		}
		else
		{
			std::printf( "\n%d of %d checks passed\n", ran - failed, ran );
			result |= ( failed == 0 ? 0 : 1 );
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
