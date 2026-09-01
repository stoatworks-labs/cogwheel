#include "Config.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>

#if defined( _WIN32 )
	#include <windows.h>
#endif

namespace cogwheel::config
{
namespace
{
constexpr const char* kAppName = "cogwheel";

std::string environmentVariable( const char* name )
{
	const char* value = std::getenv( name );
	return value ? std::string( value ) : std::string();
}

std::string homeDirectory()
{
#if defined( _WIN32 )
	return environmentVariable( "USERPROFILE" );
#else
	return environmentVariable( "HOME" );
#endif
}

/// Same shape as Diag's, but NOT the log directory. A log is something you go
/// looking for when a thing is broken; an exported configuration is something
/// you go looking for on purpose, so it goes where a person keeps files.
void createDirectories( const std::string& path )
{
#if defined( _WIN32 )
	std::string partial;
	for( char c : path )
	{
		partial += c;
		if( c == '\\' || c == '/' )
			CreateDirectoryA( partial.c_str(), nullptr );
	}
	CreateDirectoryA( path.c_str(), nullptr );
#else
	const std::string command = "mkdir -p '" + path + "'";
	(void)std::system( command.c_str() );
#endif
}
} // namespace

std::string ExportDirectory()
{
	const std::string override_ = environmentVariable( "COGWHEEL_EXPORT_DIR" );
	if( !override_.empty() )
		return override_;

	const std::string home = homeDirectory();
#if defined( _WIN32 )
	return home + "\\Documents\\" + kAppName;
#else
	return home + "/Documents/" + kAppName;
#endif
}

std::string Escape( const std::string& text )
{
	std::string out;
	out.reserve( text.size() );
	for( char c : text )
	{
		switch( c )
		{
			case '&': out += "&amp;"; break;
			case '<': out += "&lt;"; break;
			case '>': out += "&gt;"; break;
			case '"': out += "&quot;"; break;
			case '\'': out += "&apos;"; break;
			default:
				// A control character is not legal in XML 1.0 even escaped, and
				// a parameter name should never contain one -- but the display
				// string is built by the plugin and a stray one would produce a
				// file no parser will open. Drop them rather than emit them.
				if( static_cast< unsigned char >( c ) >= 0x20 || c == '\t' )
					out += c;
				break;
		}
	}
	return out;
}

std::string Document( const std::vector< Row >& rows,
                      const std::string& preset,
                      const std::string& stamp )
{
	std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
	xml += "<cogwheel exported=\"" + Escape( stamp ) + "\" preset=\"" + Escape( preset ) + "\">\n";

	// The value is what a machine needs and the display is what a person needs,
	// so both are written. `value` is the host-facing 0..1 (or the integer for
	// an INTEGER parameter) -- the same number the composition stores -- which
	// is what makes the file worth anything: it can be typed back in.
	for( const Row& row : rows )
	{
		char number[ 32 ] = {};
		std::snprintf( number, sizeof( number ), "%.6f", row.value );

		xml += "  <parameter id=\"" + std::to_string( row.id ) + "\"";
		xml += " name=\"" + Escape( row.name ) + "\"";
		xml += " type=\"" + Escape( row.type ) + "\"";
		xml += " value=\"" + std::string( number ) + "\"";
		if( !row.display.empty() )
			xml += " display=\"" + Escape( row.display ) + "\"";
		xml += "/>\n";
	}

	xml += "</cogwheel>\n";
	return xml;
}

bool Write( const std::vector< Row >& rows,
            const std::string& preset,
            std::string& pathOut,
            std::string& error )
{
	char stamp[ 32 ] = {};
	const std::time_t now = std::time( nullptr );
	std::tm broken {};
#if defined( _WIN32 )
	localtime_s( &broken, &now );
#else
	localtime_r( &now, &broken );
#endif
	std::strftime( stamp, sizeof( stamp ), "%Y-%m-%d %H:%M:%S", &broken );

	char leaf[ 64 ] = {};
	std::strftime( leaf, sizeof( leaf ), "cogwheel-%Y%m%d-%H%M%S.xml", &broken );

	const std::string directory = ExportDirectory();
	createDirectories( directory );

#if defined( _WIN32 )
	pathOut = directory + "\\" + leaf;
#else
	pathOut = directory + "/" + leaf;
#endif

	std::ofstream file( pathOut, std::ios::binary | std::ios::trunc );
	if( !file )
	{
		error = "could not open " + pathOut;
		pathOut.clear();
		return false;
	}

	const std::string document = Document( rows, preset, stamp );
	file << document;
	if( !file )
	{
		error = "could not write " + pathOut;
		pathOut.clear();
		return false;
	}

	return true;
}

} // namespace cogwheel::config
