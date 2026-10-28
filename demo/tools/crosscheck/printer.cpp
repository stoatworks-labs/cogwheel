/**
	The C++ half of the browser demo's crosscheck.

	Runs `scenario.txt` on the plugin's OWN code -- machine/Gears, Wheels,
	Crank, Pens and Clock, Controls.cpp's Resolve(), Presets.h, and the real
	CogwheelPlugin class for the display strings -- and prints everything it
	computed. `run.mjs` runs the same scenario on demo/plugin.js's PORT block
	and prints the same lines; `crosscheck.sh` requires the two to be identical.

	Floats and doubles are printed as their bit patterns, so "identical" means
	identical to the last bit, not "close". It is built -ffp-contract=off: with
	contraction on, clang fuses `a * b + c` into one fused multiply-add on arm64,
	which rounds once where the C++ source -- and JavaScript -- round twice. That
	is a property of the compiler's flags, not of the plugin's maths, and the
	crosscheck is about the maths.

	No GL is touched. The plugin class is constructed directly, as cgtest's
	--list does, and asked for its displays; the crank is driven on its own.
*/
#include "Cogwheel.h"
#include "Controls.h"
#include "Presets.h"
#include "machine/Clock.h"
#include "machine/Crank.h"
#include "machine/Gears.h"
#include "machine/Pens.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace cogwheel;

namespace
{
std::string h32( float v )
{
	uint32_t bits = 0;
	std::memcpy( &bits, &v, sizeof( bits ) );
	char buffer[ 16 ];
	std::snprintf( buffer, sizeof( buffer ), "%08x", bits );
	return buffer;
}

std::string h64( double v )
{
	uint64_t bits = 0;
	std::memcpy( &bits, &v, sizeof( bits ) );
	char buffer[ 24 ];
	std::snprintf( buffer, sizeof( buffer ), "%016llx", static_cast< unsigned long long >( bits ) );
	return buffer;
}

/// The editable controls, which is everything before the About block.
constexpr unsigned int kSlots = PT_ABOUT_TEXT;

struct State
{
	bool effect = false;
	float params[ kSlots ] = {};
	double bpm = 120.0;
	Crank crank;
	std::vector< Step > steps;
	std::vector< Run > runs;
};

uint32_t seedOf( const float* p )
{
	return static_cast< uint32_t >( std::max( 1, static_cast< int >( std::lround( p[ PT_SEED ] ) ) ) );
}

void reset( State& s, bool effect )
{
	s.effect = effect;
	CogwheelPlugin plugin( effect );
	for( unsigned int i = 0; i < kSlots; ++i )
		s.params[ i ] = plugin.GetFloatParameter( i );
	s.bpm   = 120.0;
	s.crank = Crank();
	s.crank.Restart( seedOf( s.params ) );
}

void applyPresetRow( State& s, int n )
{
	int count = 0;
	const unsigned int* ids = CogwheelPlugin::PresetParamIDsForTest( count );
	const presets::Preset& preset = presets::kPresets[ n - 1 ];
	for( int j = 0; j < count; ++j )
		s.params[ ids[ j ] ] = preset.v[ j ];
}

void printGeometry( const Geometry& g )
{
	std::printf( " g %d %d %d %d %d %d %s %s %s %s %s %s %s",
	             g.usable ? 1 : 0, g.ringTeeth, g.wheelTeeth, g.lobes, g.turnsToClose, g.commonFactor,
	             h64( g.wheelRadius ).c_str(), h64( g.orbit ).c_str(), h64( g.penRadius ).c_str(),
	             h64( g.spinPerOrbit ).c_str(), h64( g.penPhase ).c_str(),
	             h64( g.outerRadius ).c_str(), h64( g.innerRadius ).c_str() );
}

void printResolve( const Resolved& r )
{
	const CrankParams& c = r.crank;
	std::printf( "V crank %d %d %d %s %d %d %d %s %s %s %s %s %d %d %d %s %u %d %d |",
	             c.train.ringTeeth, c.train.wheelTeeth, static_cast< int >( c.train.mesh ),
	             h64( c.train.penFraction ).c_str(), c.snapWheelToSet ? 1 : 0, c.snapPenToHoles ? 1 : 0,
	             static_cast< int >( c.sync ), h64( c.turnsPerSecond ).c_str(), h64( c.bpm ).c_str(),
	             h64( c.creepTeethPerTurn ).c_str(), h64( c.skipChancePerTurn ).c_str(), h64( c.skipTeeth ).c_str(),
	             c.layers, static_cast< int >( c.change ), c.wipeOnRestack ? 1 : 0,
	             h64( c.closeWithinSeconds ).c_str(), c.seed, c.stepsPerTurn, c.paletteCount );

	const Sheet::RenderParams& p = r.render;
	std::printf( " render %s %d %s %s %s %s %s %s %d %s %s %s %s %d %d %d %s %s %s %s %d %d",
	             h32( p.flow ).c_str(), p.perDistance ? 1 : 0, h32( p.nibSigma ).c_str(), h32( p.nibSpread ).c_str(),
	             h32( p.densityFloor ).c_str(),
	             h32( p.paperColour[ 0 ] ).c_str(), h32( p.paperColour[ 1 ] ).c_str(), h32( p.paperColour[ 2 ] ).c_str(),
	             p.paperFromClip ? 1 : 0, h32( p.paperGrain ).c_str(), h32( p.tooth ).c_str(), h32( p.toothScale ).c_str(),
	             h32( p.fadeSeconds ).c_str(), p.fadeByFigure ? 1 : 0, p.negative ? 1 : 0, p.clearPaper ? 1 : 0,
	             h32( p.scale ).c_str(), h32( p.centre[ 0 ] ).c_str(), h32( p.centre[ 1 ] ).c_str(),
	             h32( p.gearLevel ).c_str(), p.inkFromClip ? 1 : 0, static_cast< int >( p.blend ) );
	std::printf( " %s %s %s %s %s %s |", h32( p.gearColour[ 0 ] ).c_str(), h32( p.gearColour[ 1 ] ).c_str(),
	             h32( p.gearColour[ 2 ] ).c_str(), h32( p.opacity ).c_str(), h32( p.passthrough ).c_str(),
	             h32( p.frameSeconds ).c_str() );
	std::printf( " detail %d pens %d palette", static_cast< int >( r.detail ), static_cast< int >( r.penSet ) );
	for( int i = 0; i < r.paletteCount; ++i )
	{
		float absorb[ 3 ];
		Absorption( r.palette + i * 3, absorb );
		std::printf( " %s,%s,%s/%s,%s,%s", h32( r.palette[ i * 3 ] ).c_str(), h32( r.palette[ i * 3 + 1 ] ).c_str(),
		             h32( r.palette[ i * 3 + 2 ] ).c_str(), h32( absorb[ 0 ] ).c_str(), h32( absorb[ 1 ] ).c_str(),
		             h32( absorb[ 2 ] ).c_str() );
	}
	std::printf( "\n" );
}

void frames( State& s, int count, double seconds, int& frameNumber )
{
	for( int f = 0; f < count; ++f )
	{
		const Resolved resolved = Resolve( s.params, s.bpm, s.effect );
		const Geometry g        = s.crank.Advance( resolved.crank, seconds, s.steps, s.runs );
		const bool wipe         = s.crank.WipeRequested();
		if( wipe )
			s.crank.ClearWipeRequest();

		std::printf( "F %d", frameNumber++ );
		printGeometry( g );
		std::printf( " c %d %d %d %s %s %s %s %zu %zu\n", s.crank.Layer(), s.crank.FiguresClosed(), wipe ? 1 : 0,
		             h64( s.crank.Theta() ).c_str(), h64( s.crank.SlipTeeth() ).c_str(),
		             h64( s.crank.FigurePhase() ).c_str(), h64( s.crank.FigureSeconds() ).c_str(),
		             s.steps.size(), s.runs.size() );
		for( const Run& run : s.runs )
			std::printf( "R %d %d %s %s %s %d %d\n", run.first, run.count, h32( run.colour[ 0 ] ).c_str(),
			             h32( run.colour[ 1 ] ).c_str(), h32( run.colour[ 2 ] ).c_str(), run.layer, run.closes ? 1 : 0 );
		for( const Step& step : s.steps )
			std::printf( "S %s %s %s %s\n", h32( step.x ).c_str(), h32( step.y ).c_str(), h32( step.ink ).c_str(),
			             h32( step.dt ).c_str() );
	}
}

/// The controls a display sweep moves and reports: everything an operator
/// sets, less New Sheet (an event that restarts the drawing), the Preset
/// dropdown (which would rewrite the others), Export XML (which writes a
/// file) and Load XML (a path).
bool swept( unsigned int id )
{
	return id != PT_RESET && id != PT_PRESET && id != PT_EXPORT && id != PT_LOAD && id < kSlots;
}

std::vector< float > sweepValues( CogwheelPlugin& plugin, unsigned int id, bool full )
{
	std::vector< float > values;
	const unsigned int type = plugin.GetParamType( id );
	if( type == FF_TYPE_INTEGER )
	{
		const RangeStruct range = plugin.GetParamRange( id );
		const int lo = static_cast< int >( range.min );
		const int hi = static_cast< int >( range.max );
		if( hi - lo > 1000 )
		{
			// Seed: 1..9999. Its display is the number; a sample is plenty.
			for( int v = lo; v <= lo + 30; ++v )
				values.push_back( static_cast< float >( v ) );
			values.push_back( static_cast< float >( hi ) );
		}
		else
		{
			for( int v = lo; v <= hi; ++v )
				values.push_back( static_cast< float >( v ) );
		}
	}
	else if( type == FF_TYPE_OPTION )
	{
		for( unsigned int v = 0; v < plugin.GetNumParamElements( id ); ++v )
			values.push_back( static_cast< float >( v ) );
	}
	else if( type == FF_TYPE_BOOLEAN )
	{
		values.push_back( 0.0f );
		values.push_back( 1.0f );
	}
	else
	{
		const int steps = full ? 100 : 20;
		for( int k = 0; k <= steps; ++k )
			values.push_back( static_cast< float >( k ) / static_cast< float >( steps ) );
	}
	return values;
}

void displays( State& s, bool full )
{
	CogwheelPlugin plugin( s.effect );
	// Custom first, so no preset is being held: the host-restatement guard
	// then has nothing to compare with and every value is written.
	plugin.SetFloatParameter( PT_PRESET, 0.0f );
	for( unsigned int id = 0; id < kSlots; ++id )
		if( swept( id ) )
			plugin.SetFloatParameter( id, s.params[ id ] );

	for( unsigned int id = 0; id < kSlots; ++id )
	{
		if( !swept( id ) )
			continue;
		for( float v : sweepValues( plugin, id, full ) )
		{
			plugin.SetFloatParameter( id, v );
			std::printf( "D %u %s ", id, h32( v ).c_str() );
			for( unsigned int shown = 0; shown < kSlots; ++shown )
			{
				if( !swept( shown ) )
					continue;
				const char* text = plugin.GetParameterDisplay( shown );
				std::printf( "%s|", text != nullptr ? text : "(null)" );
			}
			std::printf( "\n" );
			if( full )
			{
				float p[ PT_COUNT ] = {};
				for( unsigned int i = 0; i < kSlots; ++i )
					p[ i ] = plugin.GetFloatParameter( i );
				printResolve( Resolve( p, s.bpm, s.effect ) );
			}
		}
		plugin.SetFloatParameter( id, s.params[ id ] );
	}
}

void clock( const std::vector< double >& times )
{
	Clock c;
	c.SetScaleForTest( 1.0 );
	for( double t : times )
	{
		c.Update( t );
		std::printf( "K %s %s %s\n", h64( t ).c_str(), h64( c.FrameSeconds() ).c_str(), h64( c.Now() ).c_str() );
	}
}
} // namespace

int main( int argc, char** argv )
{
	if( argc < 2 )
	{
		std::fprintf( stderr, "usage: printer scenario.txt\n" );
		return 2;
	}
	std::ifstream in( argv[ 1 ] );
	if( !in )
	{
		std::fprintf( stderr, "printer: cannot read %s\n", argv[ 1 ] );
		return 2;
	}

	auto state = std::make_unique< State >();
	State& s   = *state;
	int frameNumber = 0;
	std::string line;
	while( std::getline( in, line ) )
	{
		if( line.empty() || line[ 0 ] == '#' )
			continue;
		std::istringstream words( line );
		std::string verb;
		words >> verb;
		if( verb == "scenario" )
		{
			std::string name;
			words >> name;
			std::printf( "== %s\n", name.c_str() );
			frameNumber = 0;
		}
		else if( verb == "reset" )
		{
			int effect = 0;
			words >> effect;
			reset( s, effect != 0 );
		}
		else if( verb == "preset" )
		{
			int n = 0;
			words >> n;
			applyPresetRow( s, n );
		}
		else if( verb == "set" )
		{
			unsigned int id = 0;
			std::string value;
			words >> id >> value;
			s.params[ id ] = std::strtof( value.c_str(), nullptr );
		}
		else if( verb == "bpm" )
		{
			std::string value;
			words >> value;
			s.bpm = std::strtod( value.c_str(), nullptr );
		}
		else if( verb == "restart" )
		{
			s.crank.Restart( seedOf( s.params ) );
		}
		else if( verb == "frames" )
		{
			int count = 0;
			std::string seconds;
			words >> count >> seconds;
			frames( s, count, std::strtod( seconds.c_str(), nullptr ), frameNumber );
		}
		else if( verb == "resolve" )
		{
			float p[ PT_COUNT ] = {};
			std::memcpy( p, s.params, sizeof( s.params ) );
			printResolve( Resolve( p, s.bpm, s.effect ) );
		}
		else if( verb == "displays" )
		{
			int full = 0;
			words >> full;
			displays( s, full != 0 );
		}
		else if( verb == "clock" )
		{
			std::vector< double > times;
			std::string t;
			while( words >> t )
				times.push_back( std::strtod( t.c_str(), nullptr ) );
			clock( times );
		}
		else
		{
			std::fprintf( stderr, "printer: unknown verb '%s'\n", verb.c_str() );
			return 2;
		}
	}
	return 0;
}
