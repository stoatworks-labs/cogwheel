#pragma once

#include "Controls.h"
#include "machine/Clock.h"
#include "machine/Crank.h"
#include "render/Sheet.h"

#include <FFGLSDK.h>

#include <string>
#include <vector>

namespace cogwheel
{
/**
	A Spirograph, as a machine rather than as a curve.

	## The one idea

	**Nothing evaluates a hypotrochoid.** There is a ring with N teeth, a wheel
	with n teeth rolling in mesh with it, and a pen in a hole in the wheel; the
	pen is carried, ink comes off it onto paper, and the figure is where it
	went. That is not a stylistic preference -- it is what makes every property
	the plugin has fall out for free instead of being coded:

	- **The ratio is two integers because gears mesh.** A wheel cannot roll by
	  anything other than a whole tooth, so 96/32 closes in one turn with three
	  lobes and 96/31 takes thirty-one turns and has ninety-six -- and the
	  operator can see why from the two numbers. A float "ratio" control makes
	  the same shapes and explains none of them.
	- **The gears slip, and that is what keeps the drawing alive.** A closed
	  figure retraces its own line for ever (see `machine/Crank.h`); a mesh
	  half a percent out of true precesses instead, which is both the classic
	  Spirograph failure and the only honest answer to the dead machine.
	- **Ink is subtractive.** The sheet accumulates optical density and the
	  display pass is `paper * exp( -density )`, so red over blue is the muddy
	  near-black it is on the table, a pen that lingers lays down more, and
	  changing the paper colour does not invalidate what is already drawn. See
	  `render/Shaders.h`.
	- **A drawing is a stack of figures.** When one closes you lift the pen,
	  move to another hole, change the pen and draw the next on top -- which is
	  how the multicoloured Spirograph drawing everybody remembers is actually
	  made, and it is `Layers`.

	If you are ever tempted to draw a spirograph, stop: either it already falls
	out of the machine, or the machine is wrong somewhere and that is the bug.

	## Both plugins are this class

	The source draws on its own paper; the effect can use the incoming clip as
	the paper, as the ink the pen picks up, or both. They differ by a
	constructor flag, an input count, and two controls the source ignores --
	little enough that keeping them one class is what stops them drifting apart.
	Both declare an identical parameter list so a composition can be moved
	between them without the numbering shifting underneath it.
*/
class CogwheelPlugin : public CFFGLPlugin
{
public:
	/// Declare the host clock's unit for the offline harness, which renders as
	/// fast as the GPU allows and so gives the calibration nothing to measure.
	void SetClockScaleForTest( double scale ) { clock.SetScaleForTest( scale ); }

	explicit CogwheelPlugin( bool overInput );
	~CogwheelPlugin() override = default;

	FFResult InitGL( const FFGLViewportStruct* viewPort ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	/// What a control's value MEANS, in the units it is really in. Resolume
	/// shows this instead of the raw 0..1, and FFGL offers no other
	/// per-parameter text -- there is no tooltip opcode, so this is the only
	/// place a control can explain itself.
	///
	/// It is worth more here than in most of the fleet: the closure arithmetic
	/// is the whole plugin and it is invisible. Ring and Wheel report how many
	/// turns the figure takes and how many lobes it has, which is the question
	/// an operator is actually asking when they drag either one.
	char* GetParameterDisplay( unsigned int index ) override;

	/// The value with no unit, for every control that has nothing more useful
	/// to say. **Not** a call to `CFFGLPlugin::GetParameterDisplay`: the base
	/// class reads its own `m_pPlugin` back-pointer, which is set by the SDK's
	/// C entry point when a host instantiates the plugin and is null in an
	/// instance the offline harness constructed directly. Falling through to it
	/// from `cgtest --list` is a segfault, not a blank column.
	char* PlainDisplay( unsigned int index );

	/// Write every control's current value to an XML file. #9.
	///
	/// Never throws and never fails loudly: this runs on the host's thread from
	/// inside a parameter set, and a plugin that takes Resolume down because a
	/// folder was read-only is worse than one that quietly does not export. The
	/// outcome goes to `exportNote`, which the panel shows, and the path goes to
	/// the log, which is the only place a full path fits.
	void ExportConfig();

	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	/// For the harness: the preset table's parameter ids, in the table's order.
	static const unsigned int* PresetParamIDsForTest( int& count );

	/// For the harness: what the machine currently thinks it is drawing. No GL
	/// involved, so the arithmetic can be checked without a context.
	Geometry GeometryForTest() const;

private:
	void declareParameters();
	void applyPreset( int presetIndex );
	void seedHostValues();
	bool hostIsRestatingItself( unsigned int index, float value );
	float presetValue( int presetIndex, unsigned int id ) const;

	/// A source takes no input; an effect takes exactly one. Getting this wrong
	/// is not a compile error -- the host simply files the plugin under the
	/// wrong tab and hands it a texture it was not expecting.
	const bool overInput;

	float params[ PT_COUNT ]     = {};
	float hostValues[ PT_COUNT ] = {};
	bool hostValuesSeeded        = false;

	Clock clock;
	Crank crank;
	Sheet sheet;

	/// Reused between frames so that a steady state allocates nothing. The
	/// crank clears them itself.
	std::vector< Step > steps;
	std::vector< Run > runs;

	Geometry geometry;

	/// The host is handed a bare pointer for a display value and for the About
	/// block, so both strings have to outlive the call that built them.
	std::string displayValue;

	/// What the Export XML button shows: "ready" until pressed, then the
	/// outcome. Sixteen characters is not room for a path -- see ExportConfig.
	std::string exportNote = "ready";
	std::string aboutText;

	bool glReady        = false;
	bool clearRequested = true;
	double lastHostTime = -1.0;
};

} // namespace cogwheel
