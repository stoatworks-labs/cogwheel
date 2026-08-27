#include "Wheels.h"

#include <algorithm>
#include <cmath>

namespace cogwheel
{

const int kSetWheels[] = {
	24, 30, 32, 36, 40, 42, 45, 48, 50, 52, 56, 60, 63, 64, 72, 75, 80, 84
};
const int kSetWheelCount = static_cast< int >( sizeof( kSetWheels ) / sizeof( kSetWheels[ 0 ] ) );

//Each physical ring is toothed on both edges, so the inner and outer counts of
//the same piece of plastic are both here. 96/105 is the small ring, 144/150 the
//large one.
const int kSetRings[] = { 96, 105, 144, 150 };
const int kSetRingCount = static_cast< int >( sizeof( kSetRings ) / sizeof( kSetRings[ 0 ] ) );

int NearestInTable( int value, const int* table, int count )
{
	if( table == nullptr || count <= 0 )
		return value;

	int best     = table[ 0 ];
	int bestDist = std::abs( value - best );
	for( int i = 1; i < count; ++i )
	{
		const int dist = std::abs( value - table[ i ] );
		//Strictly less, so a value exactly between two counts takes the lower
		//one every time rather than depending on the table's order.
		if( dist < bestDist )
		{
			best     = table[ i ];
			bestDist = dist;
		}
	}
	return best;
}

namespace
{
double holeRadius( int index )
{
	const double t = static_cast< double >( index ) / static_cast< double >( kHoleCount - 1 );
	return kInnermostHole + ( kOutermostHole - kInnermostHole ) * t;
}
} // namespace

int NearestHoleIndex( double fraction )
{
	const double span = kOutermostHole - kInnermostHole;
	const double t    = ( std::clamp( fraction, 0.0, 1.0 ) - kInnermostHole ) / span;
	const int index   = static_cast< int >( std::lround( t * ( kHoleCount - 1 ) ) );
	return std::clamp( index, 0, kHoleCount - 1 ) + 1;//1-based, as they are numbered
}

double NearestHole( double fraction )
{
	return holeRadius( NearestHoleIndex( fraction ) - 1 );
}

} // namespace cogwheel
