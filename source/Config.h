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
	to a file the operator can keep, read, diff and send to somebody else.

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

} // namespace cogwheel::config
