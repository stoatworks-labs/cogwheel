#pragma once

/**
	Exporting a configuration.

	#9 asked to save a liked configuration into the plugin's own preset
	dropdown. FFGL cannot do that: a dropdown's elements are declared once, in
	the constructor, and the host owns the panel afterwards -- there is no call
	that adds an element to a live plugin, and nothing that would persist one
	between sessions. Duplicating the plugin per look, which is what the
	reporter was doing, is the only in-Resolume answer and it is exactly the
	thing they were trying to stop doing.

	So this is the half that IS possible: write every control's current value
	to a file the operator can keep, read, diff and send to somebody else --
	and, since v0.3.0, read one back in through a FILE parameter, which is the
	nearest thing FFGL has to a user preset: the file is the slot.

	`Document()` is deliberately pure -- rows in, XML string out, no file
	system -- so the harness can assert the document's shape without writing
	anything, and so a failure to write is separable from a malformed file.
*/

#include <string>
#include <vector>

namespace cogwheel::config
{
/// One control, as the exported file sees it.
struct Row
{
	unsigned int id = 0;
	std::string  name;
	std::string  type;///< "standard", "integer", "option", "boolean", "event", "text"
	float        value = 0.0f;
	std::string  display;///< what the panel shows next to it, when that is a word
};

/// One control as read back from a file. Matched by NAME downstream, never by
/// id: ids move between releases -- 0.3.0 put Fade by Figure in the middle of
/// the list -- and a file, like a saved composition, has to survive that.
struct Loaded
{
	std::string name;
	float       value = 0.0f;
};

/// Where exports are written. Honours COGWHEEL_EXPORT_DIR, which is also how
/// the harness keeps its files out of a real person's Documents folder.
std::string ExportDirectory();

/// XML-escape one attribute value. Public because it is the part most likely
/// to be wrong and the part cheapest to test.
std::string Escape( const std::string& text );

/// The document, as a string. No file system.
std::string Document( const std::vector< Row >& rows,
                      const std::string& preset,
                      const std::string& stamp );

/// Write `Document()` into `ExportDirectory()`, creating it if needed.
///
/// Returns false and fills `error` on any failure. `pathOut` is the full path
/// written, which is worth logging: an operator who presses a button and sees
/// nothing has no other way to find out where the file went.
bool Write( const std::vector< Row >& rows,
            const std::string& preset,
            std::string& pathOut,
            std::string& error );

/// The inverse of `Escape`, for reading a document back.
std::string Unescape( const std::string& text );

/// Read a document `Document()` wrote. Pure: text in, rows out, no file
/// system. Anything that is not a `<parameter>` row with a name and a number
/// in it is ignored, so a hand-edited file still loads; text with no
/// `<cogwheel` root at all is refused, with `error` saying so. `preset` is
/// what the file says it was exported from, for the log.
bool Parse( const std::string& xml, std::vector< Loaded >& rows,
            std::string& preset, std::string& error );

/// The whole of one file, as text. Returns false and fills `error` if it
/// cannot be read or is empty.
bool ReadFile( const std::string& path, std::string& text, std::string& error );

} // namespace cogwheel::config
