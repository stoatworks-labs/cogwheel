#include "Gears.h"

#include <algorithm>
#include <cmath>

namespace cogwheel
{
namespace
{
constexpr double kTwoPi = 6.283185307179586;

/// The largest number of turns of the ring a figure may take before it closes.
///
/// Not a performance limit. A train like 97/96 closes after 96 turns and is a
/// perfectly good drawing; 149/150 closes after 150. What this rejects is the
/// arithmetic going somewhere useless -- a train whose closure the pen would
/// never reach inside a set anybody owns.
constexpr int kMaxTurns = 512;
} // namespace

int Gcd( int a, int b )
{
	a = std::abs( a );
	b = std::abs( b );
	while( b != 0 )
	{
		const int t = b;
		b           = a % b;
		a           = t;
	}
	return a == 0 ? 1 : a;
}

Geometry Solve( const Train& train )
{
	Geometry g;
	g.ringTeeth  = std::max( 3, train.ringTeeth );
	g.wheelTeeth = std::max( 3, train.wheelTeeth );

	const double N = static_cast< double >( g.ringTeeth );
	const double n = static_cast< double >( g.wheelTeeth );

	g.wheelRadius = n / N;

	if( train.mesh == Mesh::Inside )
	{
		//The wheel rolls round the inside, so its centre orbits at R - r and it
		//turns backwards relative to that orbit. The sign is the whole
		//difference between a hypotrochoid and an epitrochoid; everything below
		//is shared.
		g.orbit        = 1.0 - g.wheelRadius;
		g.spinPerOrbit = -g.orbit / g.wheelRadius;
		g.penPhase     = 0.0;
	}
	else
	{
		g.orbit        = 1.0 + g.wheelRadius;
		g.spinPerOrbit = g.orbit / g.wheelRadius;

		//Half a turn. Without it the outside mesh's closed form is the textbook
		//epitrochoid with its pen offset negated, which is the same figure
		//rotated -- harmless on its own, and not harmless the moment a second
		//layer is drawn on top of the first and the two are expected to line up.
		g.penPhase = 3.141592653589793;
	}

	g.penRadius = std::clamp( train.penFraction, 0.0, 1.0 ) * g.wheelRadius;

	g.commonFactor = Gcd( g.ringTeeth, g.wheelTeeth );
	g.lobes        = g.ringTeeth / g.commonFactor;
	g.turnsToClose = g.wheelTeeth / g.commonFactor;

	g.outerRadius = std::fabs( g.orbit ) + g.penRadius;
	g.innerRadius = std::fabs( std::fabs( g.orbit ) - g.penRadius );

	//A wheel bigger than the ring cannot be threaded inside it. A wheel the
	//same size as the ring has an orbit of zero and no figure at all.
	const bool fits = ( train.mesh == Mesh::Outside ) || ( g.wheelTeeth < g.ringTeeth );
	g.usable        = fits && g.turnsToClose <= kMaxTurns && g.orbit > 1.0e-6;

	return g;
}

PenPoint PenAt( const Geometry& g, double theta, double slipTeeth )
{
	//The wheel's own rotation, in radians. One slipped tooth is 2*pi/n of it,
	//and that is the ONLY way slip is allowed to enter: a tooth that jumps the
	//mesh rotates the wheel and does nothing else. Adding it to `theta`
	//instead would move the wheel round the ring without turning it, which is
	//not something two meshed gears can do.
	const double spin = g.spinPerOrbit * theta
	                  + slipTeeth * kTwoPi / static_cast< double >( g.wheelTeeth );

	const double pen = spin + g.penPhase;

	PenPoint p;
	p.x = g.orbit * std::cos( theta ) + g.penRadius * std::cos( pen );
	p.y = g.orbit * std::sin( theta ) + g.penRadius * std::sin( pen );
	return p;
}

double WheelAngle( const Geometry& g, double theta, double slipTeeth )
{
	return g.spinPerOrbit * theta
	     + slipTeeth * kTwoPi / static_cast< double >( g.wheelTeeth );
}

} // namespace cogwheel
