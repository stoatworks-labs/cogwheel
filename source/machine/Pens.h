#pragma once

namespace cogwheel
{
/**
	Pens, and what a pen actually does to paper.

	## Ink is subtractive, and this is the file that means it

	A pen does not add light to a sheet of paper; it removes it. Red ink is red
	because it absorbs green and blue, and two coats of it are darker than one
	because the light goes through twice as much of it. That is Beer's law, and
	writing it down is what makes red over blue come out the muddy near-black
	that it does on paper, rather than the bright magenta that additive blending
	would give.

	So a pen is not stored as a colour. It is stored as an **absorption
	coefficient** per channel, `k = -ln( colour )`, and the paper buffer
	accumulates `k` times how much ink was laid down. What is finally shown is

	    paper * exp( -density )

	which for one unit of a pen whose colour is C returns exactly C, for two
	units returns C squared, and for a pen crossing another returns the product
	of the two -- all three of which are what happens on the table.

	The consequence worth knowing before changing anything: **the paper buffer
	does not contain a picture.** It contains optical density. Nothing in it is
	a colour, nothing in it is clamped to 1, and it is only light in the display
	pass. That is also why changing the paper colour, or the pen palette, does
	not invalidate what has already been drawn -- the buffer never held a colour
	to be wrong about.
*/

/// Named pen sets.
enum class PenSet : int
{
	Ink = 0,   ///< One pen, the Ink colour. What a single-pen drawing is.
	Classic,   ///< Red, blue, green, black: the four refills in the box.
	Six,       ///< The larger set, with orange and purple.
	Spectrum,  ///< A hue walked across the stack of layers.
	Graphite,  ///< Pencil. One grey, and it is the one that looks most like paper.
	Count
};

/// How many pens a set offers. Spectrum is generated rather than tabulated and
/// reports the length of the stack it was asked for.
int PenCount( PenSet set, int layers );

/// Pen `index` of `set`, as a colour on white paper. `ink` is the operator's
/// own Ink colour, which is the whole answer for PenSet::Ink and is ignored by
/// the rest.
void PenColour( PenSet set, int index, int layers, const float ink[ 3 ], float out[ 3 ] );

/// Fill a palette buffer for a whole stack. `out` must hold `layers * 3`
/// floats. Returns how many entries were written.
int BuildPalette( PenSet set, int layers, const float ink[ 3 ], float* out, int capacity );

/// The absorption coefficients of a pen of this colour, for the deposit pass.
/// A colour of exactly 0 would give an infinite coefficient, so it is floored
/// at a value that is already blacker than the display can show.
void Absorption( const float colour[ 3 ], float out[ 3 ] );

} // namespace cogwheel
