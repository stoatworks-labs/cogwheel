#include "Crank.h"
#include "Wheels.h"

#include <algorithm>
#include <cmath>

namespace cogwheel
{
namespace
{
constexpr double kTwoPi = 6.283185307179586;

/// splitmix32. A hash, not a stream: every draw from it is a function of the
/// seed and the layer number alone, so the same seed lays down the same stack
/// of figures however many frames it took to get there. A stateful generator
/// would make the sequence depend on frame rate, which is the one thing the
/// whole clock exists to prevent.
uint32_t Mix( uint32_t x )
{
	x += 0x9e3779b9u;
	x = ( x ^ ( x >> 16 ) ) * 0x21f0aaadu;
	x = ( x ^ ( x >> 15 ) ) * 0x735a2d97u;
	return x ^ ( x >> 15 );
}

uint32_t Draw( uint32_t seed, int layer, int stream )
{
	return Mix( Mix( seed * 2654435761u + static_cast< uint32_t >( layer ) )
	            + static_cast< uint32_t >( stream ) * 0x85ebca6bu );
}

double Unit( uint32_t v )
{
	return static_cast< double >( v ) * ( 1.0 / 4294967296.0 );
}
} // namespace

void Crank::Restart( uint32_t seed )
{
	theta       = 0.0;
	slipTeeth   = 0.0;
	figurePhase = 0.0;
	layer       = 0;
	wipe        = false;
	seedUsed    = seed;
	rng         = seed == 0 ? 1u : seed;
}

Train Crank::CurrentTrain( const CrankParams& params ) const
{
	Train t = params.train;

	if( params.snapWheelToSet )
	{
		t.ringTeeth  = NearestInTable( t.ringTeeth, kSetRings, kSetRingCount );
		t.wheelTeeth = NearestInTable( t.wheelTeeth, kSetWheels, kSetWheelCount );
	}
	if( params.snapPenToHoles )
		t.penFraction = NearestHole( t.penFraction );

	//Layer 0 is what the operator threaded up. Every layer after it is a move
	//they made when the previous figure closed, and it is derived from the seed
	//rather than remembered, so scrubbing the transport cannot desynchronise it
	//from the picture.
	if( layer <= 0 || params.change == Change::Nothing )
		return t;

	if( params.change == Change::Wheel || params.change == Change::Both )
	{
		if( params.snapWheelToSet )
		{
			const int index = static_cast< int >( Draw( params.seed, layer, 0 ) % static_cast< uint32_t >( kSetWheelCount ) );
			t.wheelTeeth    = kSetWheels[ index ];
		}
		else
		{
			//Off the set, keep it in the same neighbourhood as what was
			//threaded: a jump from 52 teeth to 9 is not a wheel change, it is a
			//different drawing.
			const double u = Unit( Draw( params.seed, layer, 0 ) );
			const int span = std::max( 4, params.train.wheelTeeth / 2 );
			t.wheelTeeth   = std::max( 3, params.train.wheelTeeth + static_cast< int >( std::lround( ( u * 2.0 - 1.0 ) * span ) ) );
		}
		if( params.train.mesh == Mesh::Inside )
			t.wheelTeeth = std::min( t.wheelTeeth, t.ringTeeth - 1 );
	}

	if( params.change == Change::Hole || params.change == Change::Both )
	{
		if( params.snapPenToHoles )
		{
			const int index = static_cast< int >( Draw( params.seed, layer, 1 ) % static_cast< uint32_t >( kHoleCount ) );
			const double t01 = static_cast< double >( index ) / static_cast< double >( kHoleCount - 1 );
			t.penFraction    = kInnermostHole + ( kOutermostHole - kInnermostHole ) * t01;
		}
		else
		{
			t.penFraction = kInnermostHole
			              + ( kOutermostHole - kInnermostHole ) * Unit( Draw( params.seed, layer, 1 ) );
		}
	}

	return t;
}

void Crank::completeFigure( const CrankParams& params )
{
	//---------------------------------------------------------------------
	// Whether the pen comes off the paper.
	//
	// A stack of one figure is somebody who has not stopped turning. The
	// wheel never leaves the ring, so `theta` carries on from where it was
	// rather than starting again -- and, crucially, the slip carries with it.
	// That is what makes a single-wheel drawing keep laying down fresh line:
	// the mesh is a little out of true, the figure comes back a fraction of a
	// tooth rotated, and it precesses for as long as the hand keeps going.
	//
	// Reset the slip here and a one-layer drawing would retrace its own line
	// exactly for ever, which is the dead machine this whole class exists to
	// avoid. `cgtest --liveness` fails if anybody does.
	//---------------------------------------------------------------------
	const double closeAt = kTwoPi * static_cast< double >( Solve( CurrentTrain( params ) ).turnsToClose );
	const bool penLifted = std::max( 1, params.layers ) > 1;

	if( !penLifted )
	{
		theta -= closeAt;
		if( theta < 0.0 )
			theta = 0.0;
		figurePhase = 0.0;
		return;
	}

	theta       = 0.0;
	figurePhase = 0.0;

	//Slip does not carry across a lifted pen. The wheel is taken out of the
	//ring and put back, so whatever it had crept is gone -- and keeping it
	//would make every later layer of a stack progressively more rotated than
	//the one before it for no reason anybody at the table would recognise.
	slipTeeth = 0.0;

	++layer;
	if( layer >= std::max( 1, params.layers ) )
	{
		layer = 0;
		wipe  = params.wipeOnRestack;
	}
}

Geometry Crank::Advance( const CrankParams& params, double frameSeconds,
                         std::vector< Step >& steps, std::vector< Run >& runs )
{
	steps.clear();
	runs.clear();

	if( seedUsed != params.seed )
		Restart( params.seed );

	Geometry geometry = Solve( CurrentTrain( params ) );

	if( !( frameSeconds > 0.0 ) || !geometry.usable )
		return geometry;

	//---------------------------------------------------------------------
	// How fast the hand is turning.
	//
	// In Free mode that is the slider, in turns of the ring per second. In a
	// sync mode it is derived the other way round: the figure is given a
	// number of BARS to complete in, and the rate follows from how many turns
	// this particular train needs. So 96/52 -- which takes thirteen turns to
	// close -- is cranked thirteen times faster than 96/32, and both land on
	// the bar line together. That is the only sensible reading of "one figure
	// per phrase", and it is why sync cannot be a rate multiplier.
	//---------------------------------------------------------------------
	double turnsPerSecond = params.turnsPerSecond;
	if( params.sync != Sync::Free )
	{
		const double bars = params.sync == Sync::Bar1 ? 1.0
		                  : params.sync == Sync::Bar2 ? 2.0
		                  : params.sync == Sync::Bar4 ? 4.0
		                                              : 8.0;
		const double bpm         = params.bpm > 1.0 ? params.bpm : 120.0;
		const double barSeconds  = 240.0 / bpm;
		const double figureTime  = std::max( 0.05, bars * barSeconds );
		turnsPerSecond           = static_cast< double >( geometry.turnsToClose ) / figureTime;
	}

	const double closeAt = kTwoPi * static_cast< double >( geometry.turnsToClose );

	//How much of the figure this frame covers, and therefore how many steps it
	//is worth. Steps are spent per turn rather than per second, so the ink lands
	//evenly along the line however fast the hand is moving -- and the total
	//deposited is independent of the count, because each step carries its own
	//dt. `cgtest --detail` is the test.
	const double totalAdvance = std::fabs( turnsPerSecond ) * frameSeconds * kTwoPi;
	double remaining          = totalAdvance;

	int guard = 0;
	while( remaining > 1.0e-12 && guard++ < 64 )
	{
		const double toClose = std::max( 0.0, closeAt - theta );
		const double advance = std::min( remaining, toClose );

		//A whole-number step count for this run, at least two: one point is not
		//an interval and deposits nothing.
		const double turns = advance / kTwoPi;
		int count = static_cast< int >( std::ceil( turns * static_cast< double >( params.stepsPerTurn ) ) ) + 1;
		count     = std::clamp( count, 2, kMaxSteps - static_cast< int >( steps.size() ) );
		if( count < 2 )
			break;

		//This run's share of the frame. Time is apportioned by ANGLE and not by
		//step count: a run cut short by the figure closing gets the fraction of
		//the frame it actually took, so the ink it lays down is the ink that
		//much time is worth. Dividing the frame evenly between runs instead
		//would make the last stroke of a figure darker the closer the closure
		//landed to a frame boundary.
		const double runSeconds = frameSeconds * ( advance / std::max( 1.0e-12, totalAdvance ) );
		const double dt         = runSeconds / static_cast< double >( count - 1 );

		//Slip, accumulated across the run and applied per step so that a jump
		//lands inside the line rather than between two frames. Creep is
		//proportional to how far the wheel has rolled, because a mesh that is
		//not quite true is wrong by a fixed fraction of every tooth it passes.
		const double creepPerStep = params.creepTeethPerTurn * ( advance / kTwoPi ) / static_cast< double >( count - 1 );

		//A skipped tooth is a Bernoulli trial per turn, drawn from the layer's
		//own stream and indexed by which turn it is -- so the same seed skips
		//in the same places whatever the frame rate did.
		const double skipChance = std::clamp( params.skipChancePerTurn, 0.0, 1.0 );

		Run run;
		run.first = static_cast< int >( steps.size() );
		run.count = count;
		run.layer = layer;
		if( params.palette != nullptr && params.paletteCount > 0 )
		{
			const int index = layer % params.paletteCount;
			run.colour[ 0 ] = params.palette[ index * 3 + 0 ];
			run.colour[ 1 ] = params.palette[ index * 3 + 1 ];
			run.colour[ 2 ] = params.palette[ index * 3 + 2 ];
		}

		for( int i = 0; i < count; ++i )
		{
			const double t     = static_cast< double >( i ) / static_cast< double >( count - 1 );
			const double angle = theta + advance * t;

			if( i > 0 )
			{
				slipTeeth += creepPerStep;

				if( skipChance > 0.0 )
				{
					//Which turn of the ring this step is in. The trial is made
					//once per turn, at the step that crosses the turn line.
					const long thisTurn = static_cast< long >( std::floor( angle / kTwoPi ) );
					const long lastTurn = static_cast< long >( std::floor( ( theta + advance * ( static_cast< double >( i - 1 ) / static_cast< double >( count - 1 ) ) ) / kTwoPi ) );
					if( thisTurn != lastTurn )
					{
						const uint32_t draw = Draw( params.seed + static_cast< uint32_t >( thisTurn ), layer, 2 );
						if( Unit( draw ) < skipChance )
						{
							const double direction = Unit( Draw( params.seed + static_cast< uint32_t >( thisTurn ), layer, 3 ) ) < 0.5 ? -1.0 : 1.0;
							slipTeeth += direction * params.skipTeeth;
						}
					}
				}
			}

			const PenPoint p = PenAt( geometry, angle, slipTeeth );

			Step s;
			s.x   = static_cast< float >( p.x );
			s.y   = static_cast< float >( p.y );
			s.ink = 1.0f;
			//The interval that STARTS at this step. The last step of a run has
			//no interval after it, so its dt is zero and the renderer draws
			//count-1 segments.
			s.dt  = ( i + 1 < count ) ? static_cast< float >( dt ) : 0.0f;
			steps.push_back( s );
		}

		runs.push_back( run );

		theta      += advance;
		remaining  -= advance;
		figurePhase = std::clamp( theta / closeAt, 0.0, 1.0 );

		if( theta >= closeAt - 1.0e-9 )
		{
			completeFigure( params );
			geometry = Solve( CurrentTrain( params ) );
			if( !geometry.usable )
				break;
		}

		if( static_cast< int >( steps.size() ) >= kMaxSteps - 2 )
			break;
	}

	return geometry;
}

} // namespace cogwheel
