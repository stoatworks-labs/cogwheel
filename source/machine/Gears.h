#pragma once

namespace cogwheel
{
/**
	The gear train, in teeth.

	## The one idea

	**A Spirograph's ratio is a pair of integers because the gears mesh.** The
	ring has N teeth, the wheel has n, and the wheel cannot roll by anything
	other than a whole tooth at a time. Every property of the figure follows
	from N and n alone: how many lobes it has, how many turns it takes to close,
	and whether it closes at all. Nothing here takes a "ratio" as a float,
	because a float ratio is not a gear -- it is a curve, and a curve does not
	explain why 96/32 is a triangle and 96/31 is a hundred-lobed rosette.

	The pen is a hole in the wheel and rides at a fraction of the wheel's pitch
	radius. That fraction is the only continuous quantity in the machine, and on
	a real set it is not continuous either -- see `Wheels.h`.

	Radii are in units of the ring's pitch radius, so `R` is 1 by definition and
	the wheel is `n/N`. Two gears in mesh have the same module, so tooth count
	*is* radius up to that common factor, and dividing it out is what lets the
	whole machine be described by two integers and a fraction.
*/

/// Which side of the ring the wheel runs on.
enum class Mesh : int
{
	Inside = 0, ///< Hypotrochoid. The wheel rolls round the inside of the ring.
	Outside,    ///< Epitrochoid. The wheel rolls round the outside.
	Count
};

struct Train
{
	int ringTeeth   = 96;
	int wheelTeeth  = 52;
	Mesh mesh       = Mesh::Inside;
	double penFraction = 0.75; ///< Of the wheel's pitch radius, from its centre.
};

/// Everything the two tooth counts imply. Solved once per frame, never inside
/// the step loop -- `PenAt` is called thousands of times and must be arithmetic
/// only.
struct Geometry
{
	int ringTeeth  = 96;
	int wheelTeeth = 52;

	double wheelRadius = 0.0; ///< n/N, with the ring's pitch radius as 1.
	double orbit       = 0.0; ///< Radius of the wheel centre's path: R -+ r.
	double penRadius   = 0.0; ///< Pen's distance from the wheel centre.

	/// Turns of the wheel about its own centre per turn of its centre about
	/// the ring, signed: negative inside, positive outside. This is the number
	/// the whole figure is made of.
	double spinPerOrbit = 0.0;

	/// Half a turn, added to the pen's phase on the outside mesh so that the
	/// closed form matches the textbook epitrochoid. It is a hole position,
	/// not a rotation: the wheel's body angle does not carry it.
	double penPhase = 0.0;

	int commonFactor = 1; ///< gcd( N, n ).
	int lobes        = 1; ///< N / gcd. How many times the figure touches its rim.

	/// Turns of the wheel centre about the ring before the pen is back where
	/// it started, travelling the same way: n / gcd. From here on the pen
	/// retraces its own line exactly and the drawing stops changing -- which is
	/// the fact `Crank` exists to do something about.
	int turnsToClose = 1;

	double outerRadius = 0.0; ///< Furthest the pen ever gets from the centre.
	double innerRadius = 0.0; ///< Closest.

	/// True when the train is one a real set could be threaded: two meshing
	/// gears, the wheel small enough to sit inside the ring, and a figure that
	/// closes in a sane number of turns.
	bool usable = false;
};

Geometry Solve( const Train& train );

struct PenPoint
{
	double x = 0.0;
	double y = 0.0;
};

/**
	Where the pen is when the wheel's centre has gone `theta` radians round the
	ring.

	`slipTeeth` is how many teeth the wheel has slipped in the mesh since the
	start of the drawing -- see `Crank.h`. It enters as a rotation of the wheel
	about its own centre and nowhere else, because that is the only thing a
	slipped tooth can be. One tooth is a `2*pi/n` turn of the wheel.
*/
PenPoint PenAt( const Geometry& g, double theta, double slipTeeth );

/// The wheel's body angle at `theta` -- what the gear outline is drawn at. Not
/// the pen's phase: see `Geometry::penPhase`.
double WheelAngle( const Geometry& g, double theta, double slipTeeth );

/// Greatest common divisor, exposed because the harness checks the closure
/// arithmetic against it directly.
int Gcd( int a, int b );

} // namespace cogwheel
