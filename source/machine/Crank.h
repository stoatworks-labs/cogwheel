#pragma once

#include "Gears.h"
#include "Step.h"

#include <cstdint>
#include <vector>

namespace cogwheel
{
/**
	The hand on the wheel, and the drawing it is part way through.

	## Why this is not just an angle

	A gear train with the hands off is a **dead machine**, and no amount of
	tuning fixes it. The figure closes after `Geometry::turnsToClose` turns of
	the ring, and from that instant the pen retraces its own line exactly: every
	subsequent turn deposits ink where ink already is, the picture stops
	changing, and it stays stopped for ever. That is not a bug and it is not a
	rendering problem -- it is what a Spirograph *is*. A drawing is finished
	when it is finished.

	Everything in this class is one of the three things a person at the table
	actually does about that, and nothing else:

	- **The gears slip.** They really do; skipping a tooth is the classic way a
	  Spirograph drawing is ruined. A slipped tooth rotates the wheel in its
	  mesh, which precesses the whole figure, so a train that would have closed
	  keeps laying down fresh line. `Creep` is a mesh very slightly out of
	  true -- a continuous, smooth precession -- and `Skip` is the tooth that
	  jumps, discrete and startling.
	- **You change wheels.** When a figure closes you lift the pen, move to
	  another hole or another wheel, change the pen, and draw the next one on
	  top. A finished Spirograph drawing is a stack of figures, which is why
	  `Layers` is here and why each one gets its own colour.
	- **You start a new sheet.** When the stack is complete the paper is wiped
	  and the sequence begins again, from the same seed, so a composition
	  repeats rather than wandering.

	Leave all three alone and the plugin will draw one figure and then hold it,
	perfectly still, for ever. `cgtest --liveness` is the test that fails if
	anybody makes that the default.

	## What it hands to the renderer

	A block of `Step`s and the `Run`s they fall into. A run is a stretch of
	continuous pen contact in one colour; the boundary between two runs is the
	pen being lifted, and there is deliberately no segment spanning it -- which
	is how a layer change draws no line across the paper.
*/

/// What a layer change moves.
enum class Change : int
{
	Hole = 0, ///< Same wheel, next hole. The classic nested rosette.
	Wheel,    ///< Same hole, another wheel off the set.
	Both,
	Nothing,  ///< Redraw the same figure in the next colour.
	Count
};

/// Where the crank's speed comes from.
enum class Sync : int
{
	Free = 0, ///< Turns per second, straight off the slider.
	Bar1,     ///< One complete figure per bar of the host's tempo.
	Bar2,
	Bar4,
	Bar8,
	Count
};

/// A stretch of unbroken pen contact, in one colour.
struct Run
{
	int first  = 0; ///< Index of the first Step.
	int count  = 0; ///< How many Steps. A run of fewer than two draws nothing.
	float colour[ 3 ] = { 0.0f, 0.0f, 0.0f };
	int layer  = 0; ///< Which figure of the stack this is, 0-based.
};

struct CrankParams
{
	Train train;                 ///< What the operator threaded up.
	bool snapWheelToSet = false;
	bool snapPenToHoles = false;

	Sync sync              = Sync::Free;
	double turnsPerSecond  = 0.5; ///< Free mode only.
	double bpm             = 120.0;

	double creepTeethPerTurn = 0.0;   ///< A mesh that is not quite true.
	double skipChancePerTurn = 0.0;   ///< Probability of a jumped tooth, per turn.
	double skipTeeth         = 1.0;   ///< How far a jump goes.

	int layers        = 1;            ///< Figures in a stack.
	Change change     = Change::Hole;
	bool wipeOnRestack = true;        ///< Fresh sheet when the stack completes.

	uint32_t seed = 1;

	int stepsPerTurn = 1440;          ///< From Detail.

	const float* palette = nullptr;   ///< 3 floats per entry.
	int paletteCount     = 0;
};

class Crank
{
public:
	/// Back to the first layer of the stack, pen at the start of the figure.
	/// Does NOT clear the paper: that is the renderer's business, and a Reset
	/// that wiped the sheet would make the control impossible to use for
	/// "start the sequence again over what is already there".
	void Restart( uint32_t seed );

	/// Advance by `frameSeconds` and lay out this frame's pen path.
	///
	/// Returns the geometry the LAST run used, which is what the gear overlay
	/// is drawn from. Both output vectors are cleared first and are reused
	/// between frames so that a steady state allocates nothing.
	Geometry Advance( const CrankParams& params, double frameSeconds,
	                  std::vector< Step >& steps, std::vector< Run >& runs );

	/// True for exactly one frame after the stack completed and the sheet
	/// should be wiped.
	bool WipeRequested() const { return wipe; }
	void ClearWipeRequest() { wipe = false; }

	/// The figure being drawn, 0-based within the stack.
	int Layer() const { return layer; }

	/// How far through the current figure the pen is, 0..1.
	double FigurePhase() const { return figurePhase; }

	/// The wheel centre's angle about the ring, in radians, as it stands after
	/// the last Advance. The gear overlay is drawn from THIS and from the
	/// Geometry that Advance returned -- never from a second computation off
	/// the parameters, because two copies of the gear maths drift and the
	/// symptom is an overlay a fraction of a turn away from the line it is
	/// supposed to be making.
	double Theta() const { return theta; }

	/// Teeth slipped since the drawing started. Signed, and unbounded on
	/// purpose: it is a rotation, and wrapping it would put a discontinuity in
	/// the middle of a line.
	double SlipTeeth() const { return slipTeeth; }

	/// The train the current layer is actually drawing, after Snap and after
	/// the layer sequence has had its say. Not the same as `params.train`.
	Train CurrentTrain( const CrankParams& params ) const;

private:
	void completeFigure( const CrankParams& params );

	double theta       = 0.0; ///< Radians of the wheel centre about the ring.
	double slipTeeth   = 0.0;
	double figurePhase = 0.0;
	int layer          = 0;
	bool wipe          = false;
	uint32_t rng       = 1;
	uint32_t seedUsed  = 0;
};

} // namespace cogwheel
