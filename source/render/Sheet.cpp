#include "Sheet.h"

#include "Diag.h"
#include "GLState.h"
#include "Shaders.h"
#include "machine/Pens.h"

// Explicitly, and not through the umbrella: FFGLSDK.h includes every other
// ffglex header and omits this one. Leaving it out compiles right up until the
// first ScopedFBOBinding, at which point the error names a type nobody expected
// to be missing.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <string>

using namespace ffglex;

namespace cogwheel
{

bool Sheet::InitGL()
{
	struct
	{
		FFGLShader& shader;
		const std::string& vertex;
		const std::string& fragment;
		const char* name;
	} stages[] = {
		{ inkShader, shaders::inkVertex(), shaders::inkFragment(), "ink" },
		{ fadeShader, shaders::screenVertex(), shaders::fadeFragment(), "fade" },
		{ sheetShader, shaders::screenVertex(), shaders::sheetFragment(), "sheet" },
	};

	for( auto& stage : stages )
	{
		if( !stage.shader.Compile( stage.vertex, stage.fragment ) )
		{
			// Failing out of InitGL is invisible to the operator: the plugin
			// simply does nothing, with no message anywhere. Naming the stage
			// here is the only record of which one it was.
			diag::error( std::string( "shader failed to compile: " ) + stage.name
			             + " - the plugin will do nothing" );
			return false;
		}
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		return false;
	}

	glGenVertexArrays( 1, &inkVAO );
	glGenBuffers( 1, &inkVBO );
	if( inkVAO == 0 || inkVBO == 0 )
	{
		diag::error( "failed to create the ink vertex array" );
		return false;
	}

	//-----------------------------------------------------------------------
	// One buffer of Steps, read twice.
	//
	// Attribute 0 starts at the beginning and attribute 1 one element in, so
	// instance i sees step i and step i+1 with nothing duplicated and half the
	// bandwidth of an expanded segment list. It costs one thing: a draw of a
	// run of `count` steps must ask for `count - 1` instances.
	//
	// glVertexAttribDivisor is VAO state, not global state, so it has to be set
	// with this VAO bound. Set it with none bound and it lands nowhere; with
	// the wrong one bound, somewhere worse.
	//-----------------------------------------------------------------------
	glBindVertexArray( inkVAO );
	glBindBuffer( GL_ARRAY_BUFFER, inkVBO );

	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 4, GL_FLOAT, GL_FALSE, sizeof( Step ), nullptr );
	glVertexAttribDivisor( 0, 1 );

	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 4, GL_FLOAT, GL_FALSE, sizeof( Step ),
	                       reinterpret_cast< const GLvoid* >( sizeof( Step ) ) );
	glVertexAttribDivisor( 1, 1 );

	glBindVertexArray( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	//One white texel. See the note on `blankTexture`.
	glGenTextures( 1, &blankTexture );
	glBindTexture( GL_TEXTURE_2D, blankTexture );
	{
		const float white[ 4 ] = { 1.0f, 1.0f, 1.0f, 1.0f };
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, white );
	}
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );

	sheetWidth  = 0;
	sheetHeight = 0;

	return true;
}

void Sheet::DeInitGL()
{
	inkShader.FreeGLResources();
	fadeShader.FreeGLResources();
	sheetShader.FreeGLResources();

	quad.Release();

	if( inkVBO != 0 )
	{
		glDeleteBuffers( 1, &inkVBO );
		inkVBO = 0;
	}
	if( inkVAO != 0 )
	{
		glDeleteVertexArrays( 1, &inkVAO );
		inkVAO = 0;
	}

	if( blankTexture != 0 )
	{
		glDeleteTextures( 1, &blankTexture );
		blankTexture = 0;
	}

	paper.Destroy();

	sheetWidth  = 0;
	sheetHeight = 0;
}

bool Sheet::Render( const std::vector< Step >& steps, const std::vector< Run >& runs,
                    const Geometry& geometry, double theta, double slipTeeth,
                    const RenderParams& params,
                    GLuint hostFBO, const GLint viewport[ 4 ],
                    GLuint clipTexture, float maxU, float maxV )
{
	// Captured on the way in and restored however this returns, because every
	// early return below has to put the context back. FFGL requires it, and
	// Resolume renders the rest of the composition with whatever it finds -- a
	// plugin that bails out with additive blending still enabled makes the
	// NEXT effect in the chain look broken, which is where the bug report
	// comes from.
	ScopedGLState state;

	if( !inkShader.IsReady() || !fadeShader.IsReady() || !sheetShader.IsReady() )
		return false;

	const int outputWidth  = std::max( 1, viewport[ 2 ] );
	const int outputHeight = std::max( 1, viewport[ 3 ] );
	const float sheetAspect = static_cast< float >( outputWidth ) / static_cast< float >( outputHeight );

	//The source build has no clip and never will, so every sampler that would
	//have read one reads a single white texel instead.
	const GLuint clipOrBlank = clipTexture != 0 ? clipTexture : blankTexture;

	//-----------------------------------------------------------------------
	// The sheet.
	//
	// A resize reallocates, and reallocating clears -- there is no sensible way
	// to carry a drawing across a change of resolution, and a buffer's initial
	// contents are otherwise whatever texture memory the driver handed back.
	// Nothing else here is allowed to clear it: see the header.
	//-----------------------------------------------------------------------
	const bool resized = ( sheetWidth != outputWidth || sheetHeight != outputHeight );
	if( !paper.Ensure( outputWidth, outputHeight, GL_RGBA32F, PaperBuffer::Smooth ) )
	{
		diag::error( "could not allocate the sheet" );
		return false;
	}
	sheetWidth  = outputWidth;
	sheetHeight = outputHeight;

	if( params.clearSheet || resized )
		paper.ClearTo( 0.0f, 0.0f, 0.0f, 0.0f );

	//-----------------------------------------------------------------------
	// Upload the frame's steps. GL_STREAM_DRAW and a fresh glBufferData every
	// frame: the point is that the driver orphans the old storage rather than
	// waiting for the previous frame's draw to finish reading it.
	//-----------------------------------------------------------------------
	if( !steps.empty() )
	{
		glBindBuffer( GL_ARRAY_BUFFER, inkVBO );
		glBufferData( GL_ARRAY_BUFFER,
		              static_cast< GLsizeiptr >( steps.size() * sizeof( Step ) ),
		              steps.data(), GL_STREAM_DRAW );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
	}

	//-----------------------------------------------------------------------
	// 1 and 2. Fade, then deposit, into the same target.
	//-----------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( paper.GetGLID(), ScopedFBOBinding::RB_REVERT );

		// ScopedFBOBinding restores the framebuffer and says nothing about the
		// viewport. Without this the pass renders into a rectangle the size of
		// the host's output, in a buffer that may be a different size.
		glViewport( 0, 0, sheetWidth, sheetHeight );

		if( params.fadeSeconds > 0.0f && params.frameSeconds > 0.0f )
		{
			// exp( -dt / tau ), computed here rather than in the shader so that
			// it is one transcendental per frame instead of one per pixel, and
			// so that the harness can read the number back.
			const float retain = std::exp( -params.frameSeconds / params.fadeSeconds );

			setMultiplyBlend();
			ScopedShaderBinding shader( fadeShader.GetGLID() );
			fadeShader.Set( "Retain", retain );
			quad.Draw();
		}

		if( !runs.empty() && params.flow > 0.0f )
		{
			// Sum, not max(). Two strokes crossing the same texel really did
			// put twice the pigment there, and a max() would throw away every
			// crossing in the figure -- which is exactly where a spirograph
			// drawing is darkest.
			setAdditiveBlend();

			ScopedShaderBinding shader( inkShader.GetGLID() );

			inkShader.Set( "Flow", params.flow );
			inkShader.Set( "PerDistance", params.perDistance ? 1.0f : 0.0f );
			inkShader.Set( "NibSigma", std::max( params.nibSigma, 1.0e-5f ) );
			inkShader.Set( "NibSpread", std::max( params.nibSpread, 0.0f ) );
			inkShader.Set( "DensityFloor", std::max( params.densityFloor, 0.0f ) );
			inkShader.Set( "Scale", std::max( params.scale, 1.0e-4f ) );
			inkShader.Set( "Centre", params.centre[ 0 ], params.centre[ 1 ] );
			inkShader.Set( "SheetAspect", sheetAspect );
			inkShader.Set( "Tooth", std::max( params.tooth, 0.0f ) );
			inkShader.Set( "ToothScale", std::max( params.toothScale, 1.0f ) );
			inkShader.Set( "MaxUV", maxU, maxV );

			const bool clipInk = params.inkFromClip && clipTexture != 0;
			inkShader.Set( "InkFromClip", clipInk ? 1.0f : 0.0f );
			inkShader.Set( "ClipTexture", 0 );
			glActiveTexture( GL_TEXTURE0 );
			glBindTexture( GL_TEXTURE_2D, clipOrBlank );

			glBindVertexArray( inkVAO );

			for( const Run& run : runs )
			{
				const int segments = run.count - 1;
				if( segments <= 0 )
					continue;

				// Instance i of this run reads steps first+i and first+i+1, so
				// the highest index touched is first + count - 1. Asserted here,
				// at the draw, because this is where getting it wrong costs
				// something: one instance too many reads a Step past the end of
				// the run, which on most drivers returns zeroes and on some
				// returns whatever was there -- a stroke from the end of one
				// figure to somewhere arbitrary, on some machines and not others.
				assert( static_cast< size_t >( run.first + run.count ) <= steps.size()
				        && "a run runs past the end of the step block" );

				float absorb[ 3 ];
				Absorption( run.colour, absorb );
				inkShader.Set( "InkAbsorb", absorb[ 0 ], absorb[ 1 ], absorb[ 2 ] );

				// The base vertex is expressed as an attribute offset rather
				// than with glDrawArraysInstancedBaseInstance, which is GL 4.2
				// and macOS caps at 4.1.
				glBindBuffer( GL_ARRAY_BUFFER, inkVBO );
				const size_t base = static_cast< size_t >( run.first ) * sizeof( Step );
				glVertexAttribPointer( 0, 4, GL_FLOAT, GL_FALSE, sizeof( Step ),
				                       reinterpret_cast< const GLvoid* >( base ) );
				glVertexAttribPointer( 1, 4, GL_FLOAT, GL_FALSE, sizeof( Step ),
				                       reinterpret_cast< const GLvoid* >( base + sizeof( Step ) ) );

				glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, segments );
			}

			glBindBuffer( GL_ARRAY_BUFFER, 0 );
			glBindVertexArray( 0 );
			glBindTexture( GL_TEXTURE_2D, 0 );
		}
	}

	//-----------------------------------------------------------------------
	// 3. The sheet, into the host's framebuffer.
	//-----------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, hostFBO );
		glViewport( viewport[ 0 ], viewport[ 1 ], viewport[ 2 ], viewport[ 3 ] );

		setOverBlend();

		ScopedShaderBinding shader( sheetShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, paper.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, clipOrBlank );

		sheetShader.Set( "PaperTexture", 0 );
		sheetShader.Set( "ClipTexture", 1 );
		sheetShader.Set( "MaxUV", maxU, maxV );
		sheetShader.Set( "SheetAspect", sheetAspect );

		sheetShader.Set( "PaperColour", params.paperColour[ 0 ], params.paperColour[ 1 ], params.paperColour[ 2 ] );
		sheetShader.Set( "PaperFromClip", ( params.paperFromClip && clipTexture != 0 ) ? 1.0f : 0.0f );
		sheetShader.Set( "PaperGrain", std::max( params.paperGrain, 0.0f ) );
		sheetShader.Set( "ToothScale", std::max( params.toothScale, 1.0f ) );
		sheetShader.Set( "Negative", params.negative ? 1.0f : 0.0f );

		sheetShader.Set( "Opacity", std::clamp( params.opacity, 0.0f, 1.0f ) );
		sheetShader.Set( "Passthrough", clipTexture != 0 ? std::clamp( params.passthrough, 0.0f, 1.0f ) : 0.0f );

		//---------------------------------------------------------------
		// The gear overlay.
		//
		// Everything it needs is derived from the SAME Geometry and the SAME
		// theta the ink pass just drew with, rather than recomputed from the
		// parameters. A second copy of the gear maths is a second thing to
		// keep in step, and the symptom -- an overlay a fraction of a turn
		// away from the line it is supposed to be making -- is very hard to
		// see and impossible to unsee.
		//---------------------------------------------------------------
		sheetShader.Set( "GearLevel", std::clamp( params.gearLevel, 0.0f, 1.0f ) );
		sheetShader.Set( "GearColour", params.gearColour[ 0 ], params.gearColour[ 1 ], params.gearColour[ 2 ] );
		sheetShader.Set( "Scale", std::max( params.scale, 1.0e-4f ) );
		sheetShader.Set( "Centre", params.centre[ 0 ], params.centre[ 1 ] );
		sheetShader.Set( "RingRadius", 1.0f );
		sheetShader.Set( "WheelRadius", static_cast< float >( geometry.wheelRadius ) );
		sheetShader.Set( "RingTeeth", static_cast< float >( geometry.ringTeeth ) );
		sheetShader.Set( "WheelTeeth", static_cast< float >( geometry.wheelTeeth ) );

		const float wheelCx = static_cast< float >( geometry.orbit * std::cos( theta ) );
		const float wheelCy = static_cast< float >( geometry.orbit * std::sin( theta ) );
		sheetShader.Set( "WheelCentre", wheelCx, wheelCy );
		sheetShader.Set( "WheelAngle", static_cast< float >( WheelAngle( geometry, theta, slipTeeth ) ) );

		const PenPoint pen = PenAt( geometry, theta, slipTeeth );
		sheetShader.Set( "PenPoint", static_cast< float >( pen.x ), static_cast< float >( pen.y ) );

		quad.Draw();

		// Every ffglex::Scoped* binding clears to 0 on the way out rather than
		// restoring, so the units are unbound by hand and in a defined order.
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	return true;
}

} // namespace cogwheel
