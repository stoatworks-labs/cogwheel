#pragma once

#include "PaperBuffer.h"
#include "machine/Crank.h"
#include "machine/Gears.h"
#include "machine/Step.h"

#include <FFGLSDK.h>

#include <vector>

namespace cogwheel
{
/**
	The sheet of paper, and everything that happens to it.

	Three passes, and one buffer.

	  1. Fade    multiply the sheet's density in place, if it is fading at all.
	  2. Ink     one instanced quad per step interval, additive, into the same
	             buffer -- which is why there is no second buffer and no
	             combine pass. One draw call per `Run`, so that a layer change
	             gets its own pen colour without a per-step colour attribute.
	  3. Sheet   paper, grain, Beer's law, the gear overlay, and the composite
	             into the host's framebuffer.

	## The buffer

	`GL_RGBA32F`, at the output's own size, and it is **not** ping-ponged. The
	fade is drawn with `GL_ZERO, GL_SRC_COLOR`, which is a multiply in place;
	the ink is additive on top of it. A ping-pong would cost a second
	full-resolution float buffer and a full-frame copy every frame for a pass
	whose whole job is one multiply.

	32-bit and not half. The channels are summed absorptions that accumulate
	over thousands of strokes, and half-float runs out of mantissa exactly where
	a dense rosette has been building for a while: the sum stops climbing and
	the darkest part of the drawing is the part that stops responding.

	## What clears it, and what must not

	A **resize** clears it, because the sheet is the buffer and there is no
	sensible way to carry a drawing across a change of resolution. **A parameter
	change must not** -- the opposite of the fleet's usual GPU habit. The sheet
	is the drawing; wiping it because somebody nudged the pen colour would throw
	away the thing the plugin exists to make. Only Reset, and the crank's own
	end-of-stack wipe, are allowed to.

	## Traps this class is built around

	- `ffglex::ScopedFBOBinding` restores the framebuffer and **not** the
	  viewport. Every scoped bind here is followed by an explicit `glViewport`.
	- Every `ffglex::Scoped*` binding **clears to 0** on the way out rather than
	  restoring what was there. Texture units are unbound by hand.
	- `ffglex::FFGLShader::Set` has no integer-vector overload, so every size
	  and offset uniform is `vec2` in GLSL and converted here.
	- A uniform name that does not match the GLSL is silently ignored --
	  `glUniform` at location -1 is a documented no-op. No error, no warning,
	  the value simply never arrives.
*/
class Sheet
{
public:
	/// Everything the renderer needs for one frame, in physical units rather
	/// than 0..1. Mapping the host's sliders onto these is `Controls.cpp`'s
	/// job, and keeping that out of here is what lets the offline harness ask
	/// for a specific nib width rather than for a slider position.
	struct RenderParams
	{
		//--- the nib -------------------------------------------------------
		/// How much ink one step's quantum is worth. What that quantum IS
		/// depends on `perDistance`: with it set, ink per unit of paper
		/// covered; without it, ink per second of pen-down time.
		float flow      = 1.0f;
		/// A ballpoint rolls, so it lays down ink per unit of distance and a
		/// line is the same darkness however fast the hand moved. A fibre tip
		/// feeds by capillary action, per unit of time, and blooms wherever the
		/// pen slows down. See `Controls.h`.
		bool perDistance = true;
		float nibSigma  = 0.004f; ///< Paper units. 1 unit = half the sheet's height.
		float nibSpread = 0.15f;  ///< Extra sigma at full pen pressure.

		/// Peak areal density below which a segment is not drawn at all.
		/// Culling on density rather than on ink is what makes it mean
		/// something: a fast stroke and a slow one can carry identical ink and
		/// only one of them is visible.
		float densityFloor = 1.0e-5f;

		//--- the paper -----------------------------------------------------
		float paperColour[ 3 ] = { 0.94f, 0.92f, 0.86f };
		bool paperFromClip     = false;
		float paperGrain       = 0.15f; ///< How much of the tooth you can see.
		float tooth            = 0.20f; ///< How unevenly the sheet takes ink.
		float toothScale       = 220.0f;///< Grain cells per paper unit.

		/// Seconds for the drawing to fade to 1/e of itself. Zero or negative
		/// means it does not fade, which is what paper does.
		float fadeSeconds = 0.0f;

		/// Show a photographic negative of the sheet. Ink only ever darkens
		/// paper, so this is the only way to a pale line on a dark ground --
		/// and it is a real one: a drawing, photographed and printed the other
		/// way up.
		bool negative = false;

		//--- framing -------------------------------------------------------
		float scale     = 0.95f;
		float centre[ 2 ] = { 0.0f, 0.0f };

		//--- the ink -------------------------------------------------------
		bool inkFromClip = false;

		//--- the overlay ---------------------------------------------------
		float gearLevel = 0.0f;
		float gearColour[ 3 ] = { 0.55f, 0.52f, 0.48f };

		//--- output --------------------------------------------------------
		float opacity     = 1.0f;
		float passthrough = 0.0f; ///< 1 leaves the clip exactly alone.

		float frameSeconds = 1.0f / 60.0f;

		/// Wipe the sheet before drawing anything on it. For Reset, for the
		/// crank's end-of-stack, and for the first frame after a reload.
		bool clearSheet = false;
	};

	bool InitGL();
	void DeInitGL();

	/// Draw one frame.
	///
	/// `steps` is the whole frame's pen path and `runs` says how it divides
	/// into strokes; a run of fewer than two steps draws nothing, because the
	/// last step of a run has no interval after it. `geometry` and `theta` are
	/// only used by the gear overlay. `viewport` is the host's own, read fresh
	/// from GL rather than remembered from InitGL -- Resolume changes
	/// composition resolution without reinitialising a plugin.
	///
	/// `clipTexture == 0` is the source build, which has no input at all.
	bool Render( const std::vector< Step >& steps, const std::vector< Run >& runs,
	             const Geometry& geometry, double theta, double slipTeeth,
	             const RenderParams& params,
	             GLuint hostFBO, const GLint viewport[ 4 ],
	             GLuint clipTexture, float maxU, float maxV );

	/// Size of the sheet buffer, for the diagnostics log and the harness. Zero
	/// before the first successful Render.
	int SheetHeight() const { return sheetHeight; }
	int SheetWidth() const { return sheetWidth; }

private:
	ffglex::FFGLShader inkShader;
	ffglex::FFGLShader fadeShader;
	ffglex::FFGLShader sheetShader;

	ffglex::FFGLScreenQuad quad;

	PaperBuffer paper;

	GLuint inkVAO = 0;
	GLuint inkVBO = 0;

	/// One white texel, bound wherever the clip would be on the source build.
	///
	/// Not cosmetic. Both fragment stages declare `sampler2D ClipTexture`, and
	/// a core-profile driver validates every declared sampler whether the
	/// shader reaches it or not. Binding texture 0 makes it emit, per draw:
	///
	///     UNSUPPORTED (log once): POSSIBLE ISSUE: unit 0 GLD_TEXTURE_INDEX_2D
	///     is unloadable and bound to sampler type (Float)
	///
	/// which on the source build -- the one with no input, ever -- is a line in
	/// the host's log for a texture the plugin was never going to read.
	GLuint blankTexture = 0;

	int sheetWidth  = 0;
	int sheetHeight = 0;
};

} // namespace cogwheel
