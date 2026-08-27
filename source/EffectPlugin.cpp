/**
	The FF_EFFECT registration.

	See the long note in SourcePlugin.cpp: this file is listed directly in the
	CogwheelEffect target and never in cogwheel_core, because a registration in
	the shared library would put both plugins into both bundles.

	The effect adds nothing to the parameter list -- both builds declare an
	identical one, so a composition can be moved between them without the
	numbering shifting. What it adds is two controls the source ignores: the
	clip can be the paper the pen draws on, or the ink the pen picks up, or
	both.
*/
#include "Cogwheel.h"

namespace
{
class CogwheelEffect : public cogwheel::CogwheelPlugin
{
public:
	CogwheelEffect() : CogwheelPlugin( true )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< CogwheelEffect >,                      // Create method
	"CW02",                                               // Plugin unique ID of maximum length 4
	"Cogwheel Ink",                                       // Plugin name
	2,                                                    // API major version number
	1,                                                    // API minor version number
	0,                                                    // Plugin major version number
	1,                                                    // Plugin minor version number
	FF_EFFECT,                                            // Plugin type
	"Your clip, drawn with a Spirograph.\n\nInk from Clip makes the pen pick up whatever colour of the footage it is passing over, so the drawing is built out of the clip's own light rather than laid on top of it. Paper from Clip puts the footage behind the ink instead, as the sheet.\n\nHonest expectation: Ink from Clip is excellent on footage with large areas of strong colour and turns to mud on anything busy. That is what a pen does to a picture, not a setting.\n\nInk is subtractive, so it can only ever darken the clip. For a pale line on a dark ground, set Print to Negative.",// Plugin description
	"Cogwheel FFGL effect"                                // About
);

extern "C" const char* CogwheelEffectBuildStamp()
{
	return "cogwheel " COGWHEEL_VERSION " effect, built " __DATE__ " " __TIME__;
}
