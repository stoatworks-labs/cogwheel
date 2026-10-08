#include "Config.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

#if defined( _WIN32 )
	#include <windows.h>
	#include <knownfolders.h>
	#include <shlobj.h>
#else
	#include <sys/stat.h>
#endif

namespace cogwheel::config
{
namespace
{
constexpr const char* kAppName = "cogwheel";

#if defined( _WIN32 )
constexpr char kSeparator = '\\';

std::string utf8( const wchar_t* wide )
{
	if( wide == nullptr || *wide == L'\0' )
		return {};
	const int bytes = WideCharToMultiByte( CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr );
	if( bytes <= 1 )
		return {};
	std::string out( static_cast< size_t >( bytes - 1 ), '\0' );
	WideCharToMultiByte( CP_UTF8, 0, wide, -1, out.data(), bytes, nullptr, nullptr );
	return out;
}

/// UTF-8 first, and strictly: a host that sends the system code page instead
/// produces bytes that are not valid UTF-8, and reading those as the code page
/// is the only reading that finds the file.
std::wstring wide( const std::string& text )
{
	if( text.empty() )
		return {};
	for( UINT page : { static_cast< UINT >( CP_UTF8 ), static_cast< UINT >( CP_ACP ) } )
	{
		const DWORD flags = page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
		const int units   = MultiByteToWideChar( page, flags, text.c_str(), -1, nullptr, 0 );
		if( units <= 1 )
			continue;
		std::wstring out( static_cast< size_t >( units - 1 ), L'\0' );
		MultiByteToWideChar( page, flags, text.c_str(), -1, out.data(), units );
		return out;
	}
	return {};
}

std::string environmentVariable( const char* name )
{
	return utf8( _wgetenv( wide( name ).c_str() ) );
}

/// Where Documents actually is. `%USERPROFILE%\Documents` is only where it
/// starts out: OneDrive's backup moves it into `OneDrive\Documents`, an
/// organisation can redirect it, and anyone can move it from its Properties.
/// No environment variable follows it there -- the known-folder API is the one
/// source of truth, and it is what Explorer and Resolume both ask. #9: an
/// operator whose Documents was in OneDrive found exports in a folder beside
/// it that nothing else on the machine uses.
std::string documentsDirectory()
{
	PWSTR path         = nullptr;
	const HRESULT done = SHGetKnownFolderPath( FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &path );
	std::string found  = SUCCEEDED( done ) ? utf8( path ) : std::string();
	CoTaskMemFree( path );//documented as required even when the call fails
	if( !found.empty() )
		return found;

	const std::string home = environmentVariable( "USERPROFILE" );
	return home.empty() ? std::string() : home + "\\Documents";
}
#else
constexpr char kSeparator = '/';

std::string environmentVariable( const char* name )
{
	const char* value = std::getenv( name );
	return value ? std::string( value ) : std::string();
}

/// `~/Documents` is the real one on macOS -- iCloud's Desktop and Documents
/// syncs that folder where it is rather than moving it.
std::string documentsDirectory()
{
	return environmentVariable( "HOME" ) + "/Documents";
}
#endif

/// Same shape as Diag's, but NOT the log directory. A log is something you go
/// looking for when a thing is broken; an exported configuration is something
/// you go looking for on purpose, so it goes where a person keeps files.
void createDirectories( const std::string& path )
{
#if defined( _WIN32 )
	const std::wstring whole = wide( path );
	std::wstring partial;
	for( wchar_t c : whole )
	{
		partial += c;
		if( c == L'\\' || c == L'/' )
			CreateDirectoryW( partial.c_str(), nullptr );
	}
	CreateDirectoryW( whole.c_str(), nullptr );
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

	return documentsDirectory() + kSeparator + kAppName;
}

std::string PickerStart()
{
	std::string start = ExportDirectory();
	if( !start.empty() && start.back() != '/' && start.back() != '\\' )
		start += kSeparator;
	return start;
}

bool NamesAFolder( const std::string& path )
{
	if( path.empty() )
		return false;
	if( path.back() == '/' || path.back() == '\\' )
		return true;
#if defined( _WIN32 )
	const DWORD attributes = GetFileAttributesW( wide( path ).c_str() );
	return attributes != INVALID_FILE_ATTRIBUTES && ( attributes & FILE_ATTRIBUTE_DIRECTORY ) != 0;
#else
	struct stat info {};
	return stat( path.c_str(), &info ) == 0 && S_ISDIR( info.st_mode );
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

std::string Unescape( const std::string& text )
{
	std::string out;
	out.reserve( text.size() );
	for( size_t i = 0; i < text.size(); ++i )
	{
		if( text[ i ] != '&' )
		{
			out += text[ i ];
			continue;
		}
		const size_t end = text.find( ';', i );
		if( end == std::string::npos )
		{
			out += text.substr( i );
			break;
		}
		const std::string entity = text.substr( i + 1, end - i - 1 );
		if( entity == "amp" )       out += '&';
		else if( entity == "lt" )   out += '<';
		else if( entity == "gt" )   out += '>';
		else if( entity == "quot" ) out += '"';
		else if( entity == "apos" ) out += '\'';
		else                        out += text.substr( i, end - i + 1 );//not one of ours: left alone
		i = end;
	}
	return out;
}

namespace
{
/// The value of one attribute in a tag's text, unescaped. False if absent.
/// The leading space is what stops `name` matching inside `display`.
bool attribute( const std::string& tag, const char* name, std::string& value )
{
	const std::string key = std::string( " " ) + name + "=\"";
	const size_t at       = tag.find( key );
	if( at == std::string::npos )
		return false;
	const size_t from = at + key.size();
	const size_t to   = tag.find( '"', from );
	if( to == std::string::npos )
		return false;
	value = Unescape( tag.substr( from, to - from ) );
	return true;
}
} // namespace

bool Parse( const std::string& xml, std::vector< Loaded >& rows,
            std::string& preset, std::string& error )
{
	rows.clear();
	preset.clear();

	const size_t root = xml.find( "<cogwheel" );
	if( root == std::string::npos )
	{
		error = "not a cogwheel configuration: no <cogwheel> element";
		return false;
	}
	{
		const size_t end = xml.find( '>', root );
		if( end != std::string::npos )
			attribute( xml.substr( root, end - root ), "preset", preset );
	}

	size_t at = root;
	while( ( at = xml.find( "<parameter", at ) ) != std::string::npos )
	{
		const size_t end = xml.find( '>', at );
		if( end == std::string::npos )
			break;
		const std::string tag = xml.substr( at, end - at );
		at                    = end;

		std::string name;
		std::string number;
		if( !attribute( tag, "name", name ) || !attribute( tag, "value", number ) )
			continue;

		char* stop         = nullptr;
		const double value = std::strtod( number.c_str(), &stop );
		if( stop == number.c_str() || !std::isfinite( value ) )
			continue;//a row with no number in it is not a setting

		rows.push_back( Loaded{ name, static_cast< float >( value ) } );
	}
	return true;
}

bool ReadFile( const std::string& path, std::string& text, std::string& error )
{
#if defined( _WIN32 )
	std::ifstream file( wide( path ).c_str(), std::ios::binary );//MSVC's wide overload
#else
	std::ifstream file( path, std::ios::binary );
#endif
	if( !file )
	{
		error = "could not open " + path;
		return false;
	}
	std::ostringstream buffer;
	buffer << file.rdbuf();
	text = buffer.str();
	if( text.empty() )
	{
		error = "empty file: " + path;
		return false;
	}
	return true;
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

	pathOut = directory + kSeparator + leaf;

#if defined( _WIN32 )
	std::ofstream file( wide( pathOut ).c_str(), std::ios::binary | std::ios::trunc );
#else
	std::ofstream file( pathOut, std::ios::binary | std::ios::trunc );
#endif
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
