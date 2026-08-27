#include "Pens.h"

#include <algorithm>
#include <cmath>

namespace cogwheel
{
namespace
{
struct Rgb
{
	float r, g, b;
};

//The four refills. Not saturated primaries: a ballpoint's red is a dark warm
//red and its green is a bottle green, and using (1,0,0) and (0,1,0) instead is
//the single quickest way to make this look like a screensaver rather than a
//drawing.
const Rgb kClassic[] = {
	{ 0.72f, 0.10f, 0.12f },
	{ 0.11f, 0.20f, 0.62f },
	{ 0.06f, 0.42f, 0.24f },
	{ 0.08f, 0.08f, 0.10f },
};

const Rgb kSix[] = {
	{ 0.72f, 0.10f, 0.12f },
	{ 0.85f, 0.42f, 0.06f },
	{ 0.06f, 0.42f, 0.24f },
	{ 0.11f, 0.20f, 0.62f },
	{ 0.38f, 0.12f, 0.55f },
	{ 0.08f, 0.08f, 0.10f },
};

constexpr int kClassicCount = static_cast< int >( sizeof( kClassic ) / sizeof( kClassic[ 0 ] ) );
constexpr int kSixCount     = static_cast< int >( sizeof( kSix ) / sizeof( kSix[ 0 ] ) );

Rgb hue( float h )
{
	//A plain HSV sweep at the saturation and value a ballpoint actually reaches.
	const float s = 0.82f;
	const float v = 0.62f;
	const float x = h * 6.0f;
	const int i   = static_cast< int >( std::floor( x ) ) % 6;
	const float f = x - std::floor( x );
	const float p = v * ( 1.0f - s );
	const float q = v * ( 1.0f - s * f );
	const float t = v * ( 1.0f - s * ( 1.0f - f ) );
	switch( i < 0 ? i + 6 : i )
	{
		case 0: return { v, t, p };
		case 1: return { q, v, p };
		case 2: return { p, v, t };
		case 3: return { p, q, v };
		case 4: return { t, p, v };
		default: return { v, p, q };
	}
}
} // namespace

int PenCount( PenSet set, int layers )
{
	switch( set )
	{
		case PenSet::Ink:      return 1;
		case PenSet::Classic:  return kClassicCount;
		case PenSet::Six:      return kSixCount;
		case PenSet::Graphite: return 1;
		case PenSet::Spectrum: return std::max( 1, layers );
		default:               return 1;
	}
}

void PenColour( PenSet set, int index, int layers, const float ink[ 3 ], float out[ 3 ] )
{
	const int count = PenCount( set, layers );
	const int i     = count > 0 ? ( ( index % count ) + count ) % count : 0;

	Rgb c{ 0.0f, 0.0f, 0.0f };
	switch( set )
	{
		case PenSet::Ink:
			c = { ink[ 0 ], ink[ 1 ], ink[ 2 ] };
			break;
		case PenSet::Classic:
			c = kClassic[ i ];
			break;
		case PenSet::Six:
			c = kSix[ i ];
			break;
		case PenSet::Graphite:
			c = { 0.34f, 0.34f, 0.36f };
			break;
		case PenSet::Spectrum:
		default:
			c = hue( static_cast< float >( i ) / static_cast< float >( std::max( 1, count ) ) );
			break;
	}

	out[ 0 ] = c.r;
	out[ 1 ] = c.g;
	out[ 2 ] = c.b;
}

int BuildPalette( PenSet set, int layers, const float ink[ 3 ], float* out, int capacity )
{
	const int wanted = std::min( std::max( 1, layers ), capacity );
	for( int i = 0; i < wanted; ++i )
		PenColour( set, i, layers, ink, out + i * 3 );
	return wanted;
}

void Absorption( const float colour[ 3 ], float out[ 3 ] )
{
	//A floor rather than a clamp on the result. exp(-k) at k = 9.2 is 1e-4,
	//which is a thousandth of the darkest step an eight-bit display has, so a
	//pen any blacker than this is not a blacker pen -- it is a larger number
	//being carried through a float buffer that also holds the sum of every
	//other stroke on the sheet.
	constexpr float kFloor = 1.0e-4f;
	for( int i = 0; i < 3; ++i )
		out[ i ] = -std::log( std::clamp( colour[ i ], kFloor, 1.0f ) );
}

} // namespace cogwheel
