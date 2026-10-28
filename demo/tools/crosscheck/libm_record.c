/*
	Records every powf and logf the plugin's own code calls, with its result,
	so run.mjs can replay them into the port.

	Why: Controls.cpp's Exponential() is `low * std::pow( high / low, t )` in
	float, which is Apple's powf, and the page's port can only call V8's
	Math.pow (in double) and round. The two disagree by a unit or two in the
	last place in a handful of cases -- two lines of the scenario's 1.2 million
	-- which is a difference between two libms and not between the port and
	the plugin. Replaying the plugin's results lets the crosscheck demand an
	exact match everywhere else, and crosscheck.sh still reports how many lines
	differ without the replay, which is what the page itself computes.

	Built as a dylib and inserted with DYLD_INSERT_LIBRARIES. The __interpose
	section redirects every other image's calls; this one's own calls to powf
	and logf reach the real ones, which is how dyld interposition works.
*/
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* out;

static uint32_t bits( float v )
{
	uint32_t b;
	memcpy( &b, &v, sizeof( b ) );
	return b;
}

static void note( const char* name, float x, float y, float r )
{
	if( out == NULL )
	{
		const char* path = getenv( "LIBM_RECORD" );
		out = fopen( path != NULL ? path : "/dev/null", "w" );
	}
	if( out != NULL )
		fprintf( out, "%s %08x %08x %08x\n", name, bits( x ), bits( y ), bits( r ) );
}

static float record_powf( float x, float y )
{
	const float r = powf( x, y );
	note( "powf", x, y, r );
	return r;
}

static float record_logf( float x )
{
	const float r = logf( x );
	note( "logf", x, 0.0f, r );
	return r;
}

__attribute__(( used )) static struct
{
	const void* replacement;
	const void* replacee;
} interposers[] __attribute__(( section( "__DATA,__interpose" ) )) = {
	{ (const void*)record_powf, (const void*)powf },
	{ (const void*)record_logf, (const void*)logf },
};
