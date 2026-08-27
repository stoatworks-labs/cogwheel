#pragma once

namespace cogwheel
{
/**
	The tooth counts of a real set, and the holes drilled in a real wheel.

	None of this is required to draw anything: `Ring` and `Wheel` are ordinary
	integer parameters over a wide range and any pair of them meshes fine. What
	this table is for is **Snap to Set**, which restricts both to counts a
	physical Spirograph could actually be threaded with -- because the figures
	people recognise are the ones those particular integers make, and hunting
	for 96/52 by dragging a slider through 96/51 and 96/53 is not a thing an
	operator should have to do.

	### Provenance, and what is not verified

	The counts below are the ones the widely published part lists give for the
	classic sets. **They have not been checked against a physical set in hand**,
	and set contents varied between issues and territories, so treat the list as
	a convenience rather than a citation. `Ring` and `Wheel` remain free
	integers precisely so that nothing here is load-bearing: if the list is
	wrong, the plugin is not.

	The hole radii are a different case and are **openly a model**. A real wheel
	carries a row of holes running from near the rim to near the centre; their
	exact radii are a moulding detail and are not published anywhere reliable.
	`kHoleCount` positions evenly spaced between `kInnermostHole` and
	`kOutermostHole` is what Snap to Holes gives you, and the honest reason it
	exists is that the discreteness -- not the specific radii -- is what makes a
	pen position feel like a hole rather than a slider.
*/

/// Tooth counts of the wheels in the classic set, ascending.
extern const int kSetWheels[];
extern const int kSetWheelCount;

/// Tooth counts of the rings, ascending. A ring is toothed on both edges, so
/// each physical ring appears twice: once for its inner mesh and once for its
/// outer.
extern const int kSetRings[];
extern const int kSetRingCount;

/// How many holes a wheel is modelled as carrying.
constexpr int kHoleCount = 12;

/// Where the innermost and outermost holes sit, as a fraction of the wheel's
/// pitch radius. A hole at the rim would put the pen on the teeth and a hole
/// at the centre would draw a circle, so a real wheel has neither.
constexpr double kInnermostHole = 0.18;
constexpr double kOutermostHole = 0.92;

/// The nearest count in a table, for Snap to Set. Returns `value` unchanged
/// when the table is empty.
int NearestInTable( int value, const int* table, int count );

/// The nearest hole's radius fraction, for Snap to Holes.
double NearestHole( double fraction );

/// Which hole that is, 1-based, for the parameter display.
int NearestHoleIndex( double fraction );

} // namespace cogwheel
