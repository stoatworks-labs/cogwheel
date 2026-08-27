#pragma once

namespace cogwheel
{
/**
	One step of the pen along the paper.

	The unit the whole plugin is built on. A frame is not a picture of the
	figure; it is a **run of pen steps**, each with the position the nib was at
	and how long it spent getting there. Ink is deposited per step, and how
	dark a step lands is decided by that `dt` divided by the distance it
	covered -- which is what makes a slow part of the figure darker without
	anything anywhere computing a speed. See `render/Nib.h`.

	Sixteen bytes exactly, because a block of these is uploaded straight into a
	VBO and read as two `vec4` attributes offset by one element.
*/
struct Step
{
	float x   = 0.0f; ///< Paper coordinates. 1 unit = half the sheet's height.
	float y   = 0.0f;
	float ink = 1.0f; ///< 0 lifts the pen, 1 presses it home.
	float dt  = 0.0f; ///< Seconds spent on the interval that STARTS here.
};

static_assert( sizeof( Step ) == 16,
               "a Step is uploaded straight into a VBO as one vec4 attribute" );

/// The most steps one frame will ever ask for.
///
/// A figure is walked at a fixed number of steps per turn of the wheel, and a
/// hundred-turn figure cranked at a turn a frame is the worst honest case. The
/// headroom above that is for a transport scrub, which briefly asks for a
/// frame's worth of a second.
constexpr int kMaxSteps = 16384;

} // namespace cogwheel
