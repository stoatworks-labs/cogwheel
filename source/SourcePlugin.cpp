/**
	The FF_SOURCE registration.

	**This file is listed directly in the CogwheelSource target, not in
	cogwheel_core.** Both plugins share the class; what they do not share is the
	`CFFGLPluginInfo` below, and putting either registration in the shared
	library would register both plugins into both bundles.

	It is also why the shared library is an OBJECT library rather than a STATIC
	one. `CFFGLPluginInfo` registers itself from a file-scope constructor and
	nothing ever references it by name, so in an archive the linker is entitled
	to drop the whole translation unit -- giving a bundle that loads, exports
	`plugMain`, and reports that it contains no plugins.

	    nm -gU Cogwheel.bundle/Contents/MacOS/Cogwheel | grep plugMain

	`CW01`, and `CW02` for the effect. Four characters, and they must be unique
	across the whole fleet: `CG01` is already cartridge's.
*/
#include "Cogwheel.h"

namespace
{
class CogwheelSource : public cogwheel::CogwheelPlugin
{
public:
	CogwheelSource() : CogwheelPlugin( false )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< CogwheelSource >,                         // Create method
	"CW01",                                                  // Plugin unique ID of maximum length 4
	"Cogwheel",                                              // Plugin name
	2,                                                       // API major version number
	1,                                                       // API minor version number
	0,                                                       // Plugin major version number
	2,                                                       // Plugin minor version number
	FF_SOURCE,                                               // Plugin type
	"A Spirograph: a toothed ring, a wheel rolling in mesh with it, and a pen in one of the wheel's holes.\n\nNothing here evaluates a curve. Set the two tooth counts and the figure follows -- 96 and 32 mesh three to one, so that one closes in a single turn with three lobes, while 96 and 31 take thirty-one turns and have ninety-six. The Ring and Wheel controls tell you which you are about to get.\n\nInk is subtractive, so a second pen crossing the first darkens it the way it does on paper. Layers draws a stack of figures, lifting the pen and changing hole and colour each time one closes, which is how the multicoloured drawing everybody remembers is actually made.\n\nA closed figure retraces its own line for ever. Creep is a mesh very slightly out of true, and it is what keeps a drawing growing -- as it does on the real thing.\n\nStart from a Preset, at the bottom. Show Gears puts the machine on screen.",// Plugin description
	"Cogwheel FFGL source"                                   // About
);

extern "C" const char* CogwheelSourceBuildStamp()
{
	return "cogwheel " COGWHEEL_VERSION " source, built " __DATE__ " " __TIME__;
}
