#include "Cogwheel.h"

#include "Config.h"
#include "Diag.h"
#include "Presets.h"
#include "StoatworksAbout.h"
#include "StoatworksAboutParams.h"
#include "machine/Wheels.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cogwheel
{
namespace
{
/// The preset table's parameter ids, in the table's own order. The
/// static_assert below is what stops this list and `presets::Param` drifting.
constexpr unsigned int kPresetParamIDs[] = {
	PT_RING, PT_WHEEL, PT_MESH, PT_PEN, PT_SNAP_SET, PT_SNAP_HOLES,
	PT_RATE, PT_DETAIL, PT_CREEP, PT_SKIP, PT_SKIP_TEETH,
	PT_LAYERS, PT_CHANGE, PT_WIPE,
	PT_PEN_SET, PT_PEN_TYPE, PT_INK_R, PT_INK_G, PT_INK_B, PT_FLOW, PT_NIB, PT_SPREAD,
	PT_PAPER_R, PT_PAPER_G, PT_PAPER_B, PT_GRAIN, PT_TOOTH, PT_FADE, PT_PRINT,
	PT_ZOOM, PT_GEARS
};

static_assert( sizeof( kPresetParamIDs ) / sizeof( kPresetParamIDs[ 0 ] ) == presets::kParamCount,
               "the preset id list and presets::Param have gone out of step" );

static_assert( PT_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h -- "
               "add or remove a PT_ABOUT_BUTTON_n to match" );
} // namespace

CogwheelPlugin::CogwheelPlugin( bool overInput ) :
	overInput( overInput )
{
	// The source has no input; the effect takes one.
	SetMinInputs( overInput ? 1 : 0 );
	SetMaxInputs( overInput ? 1 : 0 );

	//-----------------------------------------------------------------------
	// Defaults.
	//
	// Set BEFORE the parameters are declared, because SetOptionParamInfo takes
	// the current value as its default and reads it out of this array.
	//
	// These ARE presets::kPresets[ 0 ], "Classic Rosette", written out by hand
	// -- 96 and 52 meshing with a common factor of four, four pens through four
	// holes, and a mesh a little out of true so it keeps moving. The duplication
	// is deliberate and `cgtest --defaults` is the test that fails when the two
	// go out of step. Escapement shipped a build where they had, and it opened
	// on a picture that saturated to white.
	//-----------------------------------------------------------------------
	for( int j = 0; j < presets::kParamCount; ++j )
		params[ kPresetParamIDs[ j ] ] = presets::kPresets[ 0 ].v[ j ];

	// Not covered by a preset, so not in the loop above.
	params[ PT_SYNC ]            = static_cast< float >( Sync::Free );
	params[ PT_SEED ]            = 1.0f;
	params[ PT_RESET ]           = 0.0f;
	params[ PT_CENTRE_X ]        = 0.5f;
	params[ PT_CENTRE_Y ]        = 0.5f;
	params[ PT_INK_FROM_CLIP ]   = 0.0f;
	params[ PT_PAPER_FROM_CLIP ] = 0.0f;
	//A slate grey rather than a pale one: the overlay has to read against
	//cartridge paper AND against a negative print, and a light grey vanishes
	//into the first.
	params[ PT_GEAR_R ]          = 0.36f;
	params[ PT_GEAR_G ]          = 0.40f;
	params[ PT_GEAR_B ]          = 0.46f;
	params[ PT_MIX ]             = 1.0f;
	params[ PT_PRESET ]          = 1.0f;//opens on Classic Rosette, which the defaults are

	declareParameters();

	crank.Restart( 1u );

	diag::init();
}

//---------------------------------------------------------------------------
// The parameter surface
//---------------------------------------------------------------------------

void CogwheelPlugin::declareParameters()
{
	auto standard = [ this ]( unsigned int id, const char* name ) {
		SetParamInfof( id, name, FF_TYPE_STANDARD );
	};

	auto option = [ this ]( unsigned int id, const char* name, int count, const char* const* names ) {
		SetOptionParamInfo( id, name, count, params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, i, names[ i ], static_cast< float >( i ) );
	};

	// -- Always visible ------------------------------------------------------
	//
	// Declared first and left out of every group, so no disclosure triangle can
	// hide it. See the note on PT_RESET in Controls.h.
	SetParamInfo( PT_RESET, "New Sheet", FF_TYPE_EVENT, false );

	// -- The gear train ------------------------------------------------------
	//
	// Integers with real ranges, which is the whole plugin. The STANDARD clamp
	// in SetParamInfo is guarded by the parameter type, so FF_TYPE_INTEGER
	// defaults pass through untouched and SetParamRange then widens them -- a
	// tooth count declared as a STANDARD would arrive as 1.
	SetParamInfo( PT_RING, "Ring Teeth", FF_TYPE_INTEGER, params[ PT_RING ] );
	SetParamRange( PT_RING, 12.0f, 360.0f );
	SetParamInfo( PT_WHEEL, "Wheel Teeth", FF_TYPE_INTEGER, params[ PT_WHEEL ] );
	SetParamRange( PT_WHEEL, 3.0f, 240.0f );
	option( PT_MESH, "Mesh", kMeshCount, kMeshNames );
	standard( PT_PEN, "Pen Hole" );
	SetParamInfo( PT_SNAP_SET, "Snap to Set", FF_TYPE_BOOLEAN, params[ PT_SNAP_SET ] > 0.5f );
	SetParamInfo( PT_SNAP_HOLES, "Snap to Holes", FF_TYPE_BOOLEAN, params[ PT_SNAP_HOLES ] > 0.5f );

	// -- The crank -----------------------------------------------------------
	option( PT_SYNC, "Sync", kSyncCount, kSyncNames );
	standard( PT_RATE, "Speed" );
	option( PT_DETAIL, "Detail", kDetailCount, kDetailNames );
	SetParamInfo( PT_SEED, "Seed", FF_TYPE_INTEGER, 1.0f );
	SetParamRange( PT_SEED, 1.0f, 9999.0f );

	// -- Slip ----------------------------------------------------------------
	standard( PT_CREEP, "Creep" );
	standard( PT_SKIP, "Skip Chance" );
	SetParamInfo( PT_SKIP_TEETH, "Skip Size", FF_TYPE_INTEGER, params[ PT_SKIP_TEETH ] );
	SetParamRange( PT_SKIP_TEETH, 1.0f, 12.0f );

	// -- Layers --------------------------------------------------------------
	SetParamInfo( PT_LAYERS, "Layers", FF_TYPE_INTEGER, params[ PT_LAYERS ] );
	SetParamRange( PT_LAYERS, 1.0f, static_cast< float >( kMaxLayers ) );
	option( PT_CHANGE, "On Closing", kChangeCount, kChangeNames );
	SetParamInfo( PT_WIPE, "Wipe Sheet", FF_TYPE_BOOLEAN, params[ PT_WIPE ] > 0.5f );

	// -- The pen -------------------------------------------------------------
	option( PT_PEN_SET, "Pens", kPenSetCount, kPenSetNames );
	// FF_TYPE_RED carries the swatch; green and blue are separate parameters
	// that the host groups behind it by type, which is why only the red one
	// gets a human name.
	SetParamInfof( PT_INK_R, "Ink", FF_TYPE_RED );
	SetParamInfof( PT_INK_G, "Ink_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_INK_B, "Ink_Blue", FF_TYPE_BLUE );
	option( PT_PEN_TYPE, "Pen Type", kPenTypeCount, kPenTypeNames );
	standard( PT_FLOW, "Flow" );
	standard( PT_NIB, "Nib" );
	standard( PT_SPREAD, "Pressure" );
	SetParamInfo( PT_INK_FROM_CLIP, "Ink from Clip", FF_TYPE_BOOLEAN, false );

	// -- The paper -----------------------------------------------------------
	SetParamInfof( PT_PAPER_R, "Paper", FF_TYPE_RED );
	SetParamInfof( PT_PAPER_G, "Paper_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_PAPER_B, "Paper_Blue", FF_TYPE_BLUE );
	SetParamInfo( PT_PAPER_FROM_CLIP, "Paper from Clip", FF_TYPE_BOOLEAN, false );
	standard( PT_GRAIN, "Grain" );
	standard( PT_TOOTH, "Tooth" );
	standard( PT_FADE, "Fade" );
	option( PT_PRINT, "Print", kPrintCount, kPrintNames );

	// -- Framing -------------------------------------------------------------
	standard( PT_ZOOM, "Size" );
	standard( PT_CENTRE_X, "Centre X" );
	standard( PT_CENTRE_Y, "Centre Y" );

	// -- The overlay ---------------------------------------------------------
	standard( PT_GEARS, "Show Gears" );
	SetParamInfof( PT_GEAR_R, "Gear Tint", FF_TYPE_RED );
	SetParamInfof( PT_GEAR_G, "Gear Tint_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_GEAR_B, "Gear Tint_Blue", FF_TYPE_BLUE );

	// -- Output --------------------------------------------------------------
	standard( PT_MIX, overInput ? "Mix" : "Opacity" );

	// -- Preset --------------------------------------------------------------
	{
		SetOptionParamInfo( PT_PRESET, "Preset", presets::kCount + 1, params[ PT_PRESET ] );
		SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
		for( int i = 0; i < presets::kCount; ++i )
			SetParamElementInfo( PT_PRESET, i + 1, presets::kPresets[ i ].name,
			                     static_cast< float >( i + 1 ) );
	}

	// -- Export --------------------------------------------------------------
	SetParamInfo( PT_EXPORT, "Export XML", FF_TYPE_EVENT, false );

	// -- Groups --------------------------------------------------------------
	//
	// SetParamGroup collapses RUNS of consecutive ids, so these have to match
	// the enum's order exactly. Two runs with the same name are two groups with
	// the same header, which reads as a bug.
	auto group = [ this ]( unsigned int from, unsigned int to, const char* name ) {
		for( unsigned int id = from; id <= to; ++id )
			SetParamGroup( id, name );
	};

	group( PT_RING, PT_SNAP_HOLES, "Gears" );
	group( PT_SYNC, PT_SEED, "Crank" );
	group( PT_CREEP, PT_SKIP_TEETH, "Slip" );
	group( PT_LAYERS, PT_WIPE, "Layers" );
	group( PT_PEN_SET, PT_INK_FROM_CLIP, "Pen" );
	group( PT_PAPER_R, PT_PRINT, "Paper" );
	group( PT_ZOOM, PT_CENTRE_Y, "Framing" );
	group( PT_GEARS, PT_GEAR_B, "Overlay" );
	group( PT_MIX, PT_MIX, "Output" );
	group( PT_PRESET, PT_EXPORT, "Preset" );

	// -- About ---------------------------------------------------------------
	SetParamInfo( PT_ABOUT_TEXT, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_TEXT + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	group( PT_ABOUT_TEXT, PT_COUNT - 1, "About" );
}

//---------------------------------------------------------------------------
// GL
//---------------------------------------------------------------------------

FFResult CogwheelPlugin::InitGL( const FFGLViewportStruct* )
{
	if( !sheet.InitGL() )
	{
		diag::error( "the sheet renderer would not initialise" );
		return FF_FAIL;
	}

	glReady        = true;
	clearRequested = true;
	return FF_SUCCESS;
}

FFResult CogwheelPlugin::DeInitGL()
{
	sheet.DeInitGL();
	glReady = false;
	return FF_SUCCESS;
}

FFResult CogwheelPlugin::SetTime( double time )
{
	lastHostTime = time;
	return CFFGLPlugin::SetTime( time );
}

FFResult CogwheelPlugin::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( !glReady || pGL == nullptr )
		return FF_FAIL;

	//The host's viewport, read fresh rather than remembered from InitGL:
	//Resolume changes composition resolution without reinitialising a plugin.
	GLint viewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, viewport );
	if( viewport[ 2 ] <= 0 || viewport[ 3 ] <= 0 )
		return FF_FAIL;

	GLuint clipTexture = 0;
	float maxU         = 1.0f;
	float maxV         = 1.0f;

	if( overInput )
	{
		if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
			return FF_FAIL;
		const FFGLTextureStruct& texture = *pGL->inputTextures[ 0 ];
		clipTexture                      = texture.Handle;
		const FFGLTexCoords coords       = GetMaxGLTexCoords( texture );
		maxU                             = coords.s;
		maxV                             = coords.t;
	}

	clock.Update( lastHostTime );

	Resolved resolved = Resolve( params, static_cast< double >( bpm ), overInput );

	geometry = crank.Advance( resolved.crank, clock.FrameSeconds(), steps, runs );

	//The crank's own end-of-stack wipe, and the operator's New Sheet button,
	//are the only two things besides a resize that may clear the drawing. A
	//parameter change must not -- the sheet IS the drawing, and wiping it
	//because somebody nudged the pen colour throws away the thing the plugin
	//exists to make.
	if( crank.WipeRequested() )
	{
		clearRequested = true;
		crank.ClearWipeRequest();
	}

	resolved.render.frameSeconds = static_cast< float >( clock.FrameSeconds() );
	resolved.render.clearSheet   = clearRequested;
	clearRequested               = false;

	const bool drawn = sheet.Render( steps, runs, geometry, crank.Theta(), crank.SlipTeeth(),
	                                 resolved.render,
	                                 pGL->HostFBO, viewport, clipTexture, maxU, maxV );

	return drawn ? FF_SUCCESS : FF_FAIL;
}

//---------------------------------------------------------------------------
// Parameters
//---------------------------------------------------------------------------

float CogwheelPlugin::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

FFResult CogwheelPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	seedHostValues();

	// The About buttons open a browser and store nothing, so they are handled
	// before any of the bookkeeping below: pressing one is not the operator
	// editing a control.
	if( index >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_RESET )
	{
		//An EVENT arrives as a rising edge. Both halves matter: acting on the
		//falling one too would wipe the sheet twice per press.
		if( value > 0.5f && params[ PT_RESET ] <= 0.5f )
		{
			crank.Restart( static_cast< uint32_t >( std::max( 1, static_cast< int >( std::lround( params[ PT_SEED ] ) ) ) ) );
			clearRequested = true;
		}
		params[ PT_RESET ] = value;
		return FF_SUCCESS;
	}

	if( index == PT_EXPORT )
	{
		//A rising edge, exactly like New Sheet above: a held event would write a
		//file per frame.
		if( value > 0.5f && params[ PT_EXPORT ] <= 0.5f )
			ExportConfig();
		params[ PT_EXPORT ] = value;
		return FF_SUCCESS;
	}

	if( index == PT_PRESET )
	{
		const int chosen = static_cast< int >( std::lround( value ) );
		if( chosen != static_cast< int >( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	// The host may be restating a value it still believes in rather than the
	// operator moving anything. Letting that through would overwrite the
	// preset's value in params[] AND read as an edit, dropping the dropdown
	// back to Custom -- which is what made presets look like they could not be
	// selected at all in Resolume. See AGENTS.md.
	if( hostIsRestatingItself( index, value ) )
		return FF_SUCCESS;

	const float previous = params[ index ];
	params[ index ]      = value;

	const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
	if( active > 0 && std::fabs( value - previous ) > 1e-4f )
	{
		for( unsigned int id : kPresetParamIDs )
		{
			if( id == index )
			{
				// Logged, unlike an ordinary parameter change: this one is a
				// state change an operator can be surprised by, it happens once
				// rather than per frame, and diagnosing the equivalent bug in
				// vertigo needed a code read precisely because nothing said it
				// had happened.
				diag::info( "preset dropped to Custom: parameter "
				            + std::to_string( index ) + " moved to "
				            + std::to_string( value ) );
				params[ PT_PRESET ] = 0.0f;
				RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
				break;
			}
		}
	}

	return FF_SUCCESS;
}

const unsigned int* CogwheelPlugin::PresetParamIDsForTest( int& count )
{
	count = presets::kParamCount;
	return kPresetParamIDs;
}

Geometry CogwheelPlugin::GeometryForTest() const
{
	const Resolved resolved = Resolve( params, static_cast< double >( bpm ), overInput );
	return Solve( crank.CurrentTrain( resolved.crank ) );
}

float CogwheelPlugin::presetValue( int presetIndex, unsigned int id ) const
{
	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return -1.0f;

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
		if( kPresetParamIDs[ j ] == id )
			return preset.v[ j ];

	return -1.0f;
}

void CogwheelPlugin::seedHostValues()
{
	// Seeded on first parameter traffic rather than in the constructor, so the
	// whole mechanism stays in one place. It has to happen BEFORE applyPreset
	// can run: seeding afterwards would record the preset's own values as the
	// host's opening position, and the host's very next restatement would then
	// look like an edit -- which is the bug this exists to fix, reintroduced.
	if( hostValuesSeeded )
		return;

	for( unsigned int i = 0; i < PT_COUNT; ++i )
		hostValues[ i ] = params[ i ];
	hostValuesSeeded = true;
}

bool CogwheelPlugin::hostIsRestatingItself( unsigned int index, float value )
{
	const float lastFromHost = hostValues[ index ];
	hostValues[ index ]      = value;

	const float fromPreset =
		presetValue( static_cast< int >( std::lround( params[ PT_PRESET ] ) ), index );
	if( fromPreset < 0.0f )
		return false;

	// A quantisation allowance rather than a float epsilon. A host that keeps
	// its parameters shorter than a float -- or round-trips them through a UI,
	// a MIDI value or a saved composition -- hands back a number near ours
	// rather than ours, and 1e-4 read that as an edit.
	//
	// Deliberately a LOOSER number than the 1e-4 used below for "did a covered
	// parameter move?". A value that matches the preset must be ignored rather
	// than written: writing a host's rounded copy of our own value would trip
	// the tighter test.
	constexpr float kSame = 1e-3f;

	if( std::fabs( value - fromPreset ) <= kSame )
		return true;

	if( std::fabs( value - lastFromHost ) > kSame )
		return false;//neither: the operator has taken over

	// Deliberately not logged. A host that pushes its parameters every frame
	// would put a line here every frame, and a log that scrolls is a log
	// nobody reads.
	return true;
}

void CogwheelPlugin::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = static_cast< float >( presetIndex );

	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return;//Custom: the sliders keep whatever they said

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		// The copy is what changes the drawing; the event only tells the host
		// to re-read the slider. A host that ignores it renders the preset
		// correctly and merely shows stale knobs.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}

	// A preset is a different machine, not a different setting on this one, so
	// the drawing in progress is finished with. This is one of the three things
	// allowed to wipe the sheet.
	crank.Restart( static_cast< uint32_t >( std::max( 1, static_cast< int >( std::lround( params[ PT_SEED ] ) ) ) ) );
	clearRequested = true;

	diag::info( std::string( "preset applied: " ) + preset.name );
}

//---------------------------------------------------------------------------
// Text
//---------------------------------------------------------------------------

char* CogwheelPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_TEXT )
	{
		// Function-local rather than a member: the line is built from
		// compile-time facts, so it is the same for every instance, and the
		// host only needs the pointer to outlive the call.
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult CogwheelPlugin::SetTextParameter( unsigned int index, const char* value )
{
	// Must return FF_SUCCESS for the About block, or no host can instantiate
	// the plugin at all: the base class fails an unknown text parameter, and a
	// host that sets one during instantiation treats that as the plugin
	// refusing to load.
	if( index == PT_ABOUT_TEXT )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

//---------------------------------------------------------------------------
// What a control means
//---------------------------------------------------------------------------

char* CogwheelPlugin::GetParameterDisplay( unsigned int index )
{
	if( index >= PT_COUNT )
		return nullptr;

	//Resolved from `params` rather than from anything the render thread keeps,
	//because the host asks for this on its own thread: a cache filled during
	//ProcessOpenGL always shows the previous setting.
	const Resolved resolved = Resolve( params, static_cast< double >( bpm ), overInput );
	const Train train       = crank.CurrentTrain( resolved.crank );
	const Geometry g        = Solve( train );

	// ☠️ EVERY STRING BELOW MUST RENDER IN 16 CHARACTERS OR FEWER, AT ITS
	// WIDEST VALUE. FF_GET_PARAMETER_DISPLAY hands the host a 16-byte buffer --
	// the SDK's own default writes into `static char s_DisplayValue[ 16 ]` --
	// and Resolume copies 16 bytes with no terminator. Nothing plugin-side ever
	// notices: this buffer is 96, snprintf succeeds, the harness prints the
	// whole string, and only the operator sees it cut. That is how "0.63% of
	// the sheet" reached a release reading "0.63% of the she".
	//
	// Widest values, for checking a new string against: ring 360, wheel 240,
	// lobes 360, turns 512, holes 12, layers 16.
	char buffer[ 96 ] = {};
	switch( index )
	{
	case PT_RING:
		//The two counts report the CLOSURE, not themselves. That arithmetic is
		//the whole plugin and it is otherwise invisible: an operator dragging
		//either of these is asking "what shape is this going to be?", and the
		//lobe count and the turn count are the answer.
		std::snprintf( buffer, sizeof( buffer ), "%dt - %d lobes",
		               train.ringTeeth, g.lobes );//360t - 360 lobes = 16
		break;
	case PT_WHEEL:
		//"closes" rather than "closes in N turns": the verb is what carries the
		//meaning and the sentence does not fit.
		std::snprintf( buffer, sizeof( buffer ), "%dt closes %d",
		               train.wheelTeeth, g.turnsToClose );//240t closes 512 = 15
		break;
	case PT_PEN:
		if( resolved.crank.snapPenToHoles )
			std::snprintf( buffer, sizeof( buffer ), "hole %d of %d",
			               NearestHoleIndex( train.penFraction ), kHoleCount );
		else
			std::snprintf( buffer, sizeof( buffer ), "%.0f%% of wheel",
			               100.0 * train.penFraction );//100% of wheel = 13
		break;
	case PT_RATE:
		if( resolved.crank.sync != Sync::Free )
			std::snprintf( buffer, sizeof( buffer ), "set by Sync" );
		else
			//The seconds-a-figure figure went with the truncation: at a slow
			//crank it runs to five digits, and no phrasing of both numbers fits
			//in 16. The rate is the one the slider is actually setting.
			std::snprintf( buffer, sizeof( buffer ), "%.2f turns/s",
			               resolved.crank.turnsPerSecond );//10.00 turns/s = 13
		break;
	case PT_CREEP:
		std::snprintf( buffer, sizeof( buffer ), "%.2f teeth/turn",
		               resolved.crank.creepTeethPerTurn );//1.00 teeth/turn = 15
		break;
	case PT_SKIP:
		if( resolved.crank.skipChancePerTurn <= 0.0 )
			std::snprintf( buffer, sizeof( buffer ), "never" );
		else
			std::snprintf( buffer, sizeof( buffer ), "%.0f%% a turn", 100.0 * resolved.crank.skipChancePerTurn );
		break;
	case PT_NIB:
		//As a fraction of the sheet's height, which is the only measure that
		//means the same thing at every output resolution.
		std::snprintf( buffer, sizeof( buffer ), "%.2f%% of sheet",
		               100.0f * resolved.render.nibSigma * 0.5f );//2.00% of sheet = 14
		break;
	case PT_FADE:
		if( resolved.render.fadeSeconds <= 0.0f )
			std::snprintf( buffer, sizeof( buffer ), "never - paper" );
		else
			std::snprintf( buffer, sizeof( buffer ), "%.0f s", resolved.render.fadeSeconds );
		break;
	case PT_FLOW:
		//The units differ between the two pens, and saying which is the whole
		//value of the display here: a number that means "per millimetre of
		//line" and a number that means "per second" are not comparable, and the
		//operator has just switched between them.
		//"a unit"/"a second" rather than "per unit drawn"/"per second": the
		//distinction is the whole point of the display and survives the
		//shortening, where "per unit drawn" did not fit at all.
		std::snprintf( buffer, sizeof( buffer ), "%.3f %s", resolved.render.flow,
		               resolved.render.perDistance ? "a unit" : "a second" );//999.999 a second = 16
		break;
	case PT_ZOOM:
		std::snprintf( buffer, sizeof( buffer ), "%.2fx", resolved.render.scale );
		break;
	case PT_LAYERS:
		if( resolved.crank.layers <= 1 )
			std::snprintf( buffer, sizeof( buffer ), "1 - never lifts" );
		else
			std::snprintf( buffer, sizeof( buffer ), "%d figures", resolved.crank.layers );
		break;
	case PT_EXPORT:
		//"ready" until it has been pressed, then the outcome. 16 characters is
		//not room for a path, which is why the path goes in the log.
		std::snprintf( buffer, sizeof( buffer ), "%s", exportNote.c_str() );//failed - see log = 16
		break;
	default:
		return PlainDisplay( index );
	}

	displayValue = buffer;
	return const_cast< char* >( displayValue.c_str() );
}

void CogwheelPlugin::ExportConfig()
{
	std::vector< config::Row > rows;
	rows.reserve( PT_EXPORT );

	// Everything an operator can set, and nothing they cannot. The About block
	// is excluded on purpose: a text line and four buttons that open a browser
	// are not configuration, and reading a button's value is meaningless.
	for( unsigned int id = 0; id < PT_ABOUT_TEXT; ++id )
	{
		config::Row row;
		row.id = id;

		const char* name = GetParamName( id );
		row.name         = name != nullptr ? name : "";
		row.value        = params[ id ];

		switch( GetParamType( id ) )
		{
			case FF_TYPE_BOOLEAN: row.type = "boolean"; break;
			case FF_TYPE_EVENT:   row.type = "event"; break;
			case FF_TYPE_INTEGER: row.type = "integer"; break;
			case FF_TYPE_OPTION:  row.type = "option"; break;
			case FF_TYPE_TEXT:    row.type = "text"; break;
			case FF_TYPE_RED:
			case FF_TYPE_GREEN:
			case FF_TYPE_BLUE:    row.type = "colour"; break;
			default:              row.type = "standard"; break;
		}

		// The display is the human half of the row -- "96t - 5 lobes" says more
		// about a machine than 0.234 does. Skipped for the export button itself,
		// whose display is about the export that has not happened yet.
		if( id != PT_EXPORT )
		{
			const char* display = GetParameterDisplay( id );
			row.display         = display != nullptr ? display : "";
		}

		rows.push_back( std::move( row ) );
	}

	const int active = Option( params[ PT_PRESET ], 1 + presets::kCount );
	const std::string preset =
		active >= 1 && active <= presets::kCount ? presets::kPresets[ active - 1 ].name : "Custom";

	std::string path;
	std::string error;
	if( config::Write( rows, preset, path, error ) )
	{
		exportNote = "saved";
		// The path is the whole point of logging this. An operator presses a
		// button, the panel says "saved", and without this line there is nothing
		// anywhere that says WHERE -- which is the same as not having saved it.
		diag::info( "configuration exported to " + path );
	}
	else
	{
		exportNote = "failed - see log";
		diag::error( "configuration export failed: " + error );
	}
}

char* CogwheelPlugin::PlainDisplay( unsigned int index )
{
	if( index >= PT_COUNT )
		return nullptr;

	const unsigned int type = GetParamType( index );
	if( type == FF_TYPE_TEXT || type == FF_TYPE_FILE )
		return GetTextParameter( index );

	char buffer[ 64 ] = {};
	if( type == FF_TYPE_OPTION )
	{
		//The element's NAME, not its index. An option's display is the one place
		//an operator can check that a dropdown is where they think it is, and a
		//bare "3" is not that.
		const unsigned int element = static_cast< unsigned int >(
			std::max( 0L, std::lround( params[ index ] ) ) );
		const char* name = element < GetNumParamElements( index )
		                     ? GetParamElementName( index, element )
		                     : nullptr;
		std::snprintf( buffer, sizeof( buffer ), "%s", name != nullptr ? name : "?" );
	}
	else if( type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT )
	{
		std::snprintf( buffer, sizeof( buffer ), "%s", params[ index ] > 0.5f ? "on" : "off" );
	}
	else if( type == FF_TYPE_INTEGER )
	{
		std::snprintf( buffer, sizeof( buffer ), "%ld", std::lround( params[ index ] ) );
	}
	else
	{
		std::snprintf( buffer, sizeof( buffer ), "%.4f", params[ index ] );
	}

	displayValue = buffer;
	return const_cast< char* >( displayValue.c_str() );
}

} // namespace cogwheel
