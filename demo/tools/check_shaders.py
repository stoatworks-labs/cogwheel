"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py           compare; exit 1 on any drift
    python3 demo/tools/check_shaders.py --write   regenerate demo/shaders.js

Called from `tools/verify.sh`. Exit code 1 means a copy has drifted.

------------------------------------------------------------------- why

`demo/shaders.js` holds the eight GLSL pieces of `source/render/Shaders.cpp`:
kConstants (the version line and the shared constants), kFragmentHelpers (the
normal CDF and the paper's tooth), and the screen-quad vertex stage, the ink
pass's two stages, the fade, the compose and the sheet. That is two copies of
the same text, and two copies drift -- quietly, because a line drawn by a
*plausible* ink shader looks exactly like one drawn by the right one. The whole
claim of the page is that it runs the plugin's own shaders, so the claim needs
something enforcing it. `cgtest` drives the real plugin class and has never
heard of this page, and verify.sh's glslc step compiles the C++ copies and never
looks at the JS one.

------------------------------------------------------------------- what it does

Pulls each piece out of `source/render/Shaders.cpp` and the matching backtick
literal out of `demo/shaders.js`, and compares them exactly -- no whitespace
normalisation, no comment stripping: a comment updated on one side only is
exactly the drift worth catching.

It also fails when Shaders.cpp grows a piece this script does not list (a new
pass the page would silently lack), and it checks the JOIN as well as the
pieces: `vertexSource()` is kConstants + the body, `fragmentSource()` is
kConstants + kFragmentHelpers + the body, and `Sheet::InitGL` pairs them into
four programs. plugin.js must assemble and pair them the same way. A change to
either side's assembly fails here and needs a reader.

The one transformation is a decode, not a normalisation: a backtick cannot
appear raw inside a template literal, so --write would escape one as \\`. There
are none in the C++ today. This undoes that escape and REJECTS any other
backslash or any `${` on the JS side; there are none in the C++ bodies, so either
could only be somebody hiding a difference.

`--write` produces shaders.js from the C++ by exactly that escape, so the copy
is spliced by this script and never typed.

------------------------------------------------------------------- what it cannot

Nothing here checks the PORTED half. The machine (machine/Gears, Wheels, Crank,
Pens), the mappings and the presets (Controls.cpp, Presets.h) and the display
strings (Cogwheel.cpp's GetParameterDisplay) are a hand translation in
demo/plugin.js, held to the C++ by demo/tools/crosscheck/, which verify.sh also
runs. Sheet::Render's GL calls -- the passes, the blending, the per-run draws,
the uniforms -- are a hand translation that nothing here checks; only a reader
does, and the frame comparison in AGENTS.md. When you change one of those,
change the port too.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.dont_write_bytecode = True

SHADERS_CPP = "source/render/Shaders.cpp"

# JS constant, C++ symbol (all in source/render/Shaders.cpp), in the order they
# are declared there.
SHADERS = [
    ("CONSTANTS", "kConstants"),
    ("FRAGMENT_HELPERS", "kFragmentHelpers"),
    ("SCREEN_VERTEX_BODY", "kScreenVertexBody"),
    ("INK_VERTEX_BODY", "kInkVertexBody"),
    ("INK_FRAGMENT_BODY", "kInkFragmentBody"),
    ("FADE_FRAGMENT_BODY", "kFadeFragmentBody"),
    ("COMPOSE_FRAGMENT_BODY", "kComposeFragmentBody"),
    ("SHEET_FRAGMENT_BODY", "kSheetFragmentBody"),
]

# How the pieces are joined and paired, on each side. Each entry is (file, text
# that must appear in it verbatim). The C++ lines are the plugin's assembly; the
# JS lines are plugin.js's copy of it. A change to either is a change to what
# runs.
ASSEMBLY = [
    (SHADERS_CPP,
     "std::string vertexSource( const char* body )\n{\n\tstd::string source = kConstants;\n"
     "\tsource += body;\n\treturn source;\n}"),
    (SHADERS_CPP,
     "std::string fragmentSource( const char* body )\n{\n\tstd::string source = kConstants;\n"
     "\tsource += kFragmentHelpers;\n\tsource += body;\n\treturn source;\n}"),
    (SHADERS_CPP, "static const std::string source = vertexSource( kScreenVertexBody );"),
    (SHADERS_CPP, "static const std::string source = vertexSource( kInkVertexBody );"),
    (SHADERS_CPP, "static const std::string source = fragmentSource( kInkFragmentBody );"),
    (SHADERS_CPP, "static const std::string source = fragmentSource( kFadeFragmentBody );"),
    (SHADERS_CPP, "static const std::string source = fragmentSource( kComposeFragmentBody );"),
    (SHADERS_CPP, "static const std::string source = fragmentSource( kSheetFragmentBody );"),
    ("source/render/Sheet.cpp", '{ inkShader, shaders::inkVertex(), shaders::inkFragment(), "ink" },'),
    ("source/render/Sheet.cpp", '{ fadeShader, shaders::screenVertex(), shaders::fadeFragment(), "fade" },'),
    ("source/render/Sheet.cpp", '{ composeShader, shaders::screenVertex(), shaders::composeFragment(), "compose" },'),
    ("source/render/Sheet.cpp", '{ sheetShader, shaders::screenVertex(), shaders::sheetFragment(), "sheet" },'),
    ("demo/plugin.js", "const vertexSource = (body) => S.CONSTANTS + body;"),
    ("demo/plugin.js", "const fragmentSource = (body) => S.CONSTANTS + S.FRAGMENT_HELPERS + body;"),
    ("demo/plugin.js", "ink: new Program(gl, vertexSource(S.INK_VERTEX_BODY), fragmentSource(S.INK_FRAGMENT_BODY), 'ink', inkAttribs),"),
    ("demo/plugin.js", "fade: new Program(gl, vertexSource(S.SCREEN_VERTEX_BODY), fragmentSource(S.FADE_FRAGMENT_BODY), 'fade'),"),
    ("demo/plugin.js", "compose: new Program(gl, vertexSource(S.SCREEN_VERTEX_BODY), fragmentSource(S.COMPOSE_FRAGMENT_BODY), 'compose'),"),
    ("demo/plugin.js", "sheet: new Program(gl, vertexSource(S.SCREEN_VERTEX_BODY), fragmentSource(S.SHEET_FRAGMENT_BODY), 'sheet'),"),
]

HEADER = """// GENERATED from source/render/Shaders.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape allowed is \\` for a backtick inside a comment.
"""


def read(*parts):
    with open(os.path.join(REPO, *parts)) as handle:
        return handle.read()


def from_cpp(source, symbol):
    """The piece's text: its raw strings, joined as the compiler joins them.

    Returns (text, count of raw strings), or (None, 0) if the symbol is not
    there or what follows its `=` is not a run of adjacent raw strings ending
    in `;`.
    """
    match = re.search(r'const char\* const ' + symbol + r' = ((?:R"\(.*?\)"\s*)+);', source, re.S)
    if match is None:
        return None, 0
    parts = re.findall(r'R"\((.*?)\)"', match.group(1), re.S)
    rebuilt = re.sub(r'R"\(.*?\)"', "", match.group(1), flags=re.S)
    if rebuilt.strip() != "":
        return None, 0
    return "".join(parts), len(parts)


def cpp_pieces(source):
    """Every raw-string piece Shaders.cpp defines, by symbol."""
    return re.findall(r'const char\* const (k\w+) = R"\(', source)


def from_js(source, name):
    match = re.search(r'^export const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        line = body[: stray.start()].count("\n") + 1
        return None, f"backslash that is not an escaped backtick, at line {line}"
    if "${" in body:
        return None, "template substitution"
    return body.replace("\\`", "`"), None


def write(cpp):
    out = [HEADER]
    for name, symbol in SHADERS:
        body, count = from_cpp(cpp, symbol)
        if body is None:
            print(f"FAIL  {symbol} not found in {SHADERS_CPP} (or not a run of adjacent raw strings)")
            return 1
        if "\\" in body or "${" in body:
            print(f"FAIL  {symbol} holds a backslash or ${{; the escape scheme cannot carry it")
            return 1
        joined = f", {count} adjacent raw strings joined" if count > 1 else ""
        out.append(f"\n// {symbol}, {SHADERS_CPP}{joined}\nexport const {name} = `{body.replace('`', chr(92) + '`')}`;\n")
    with open(os.path.join(REPO, "demo", "shaders.js"), "w") as handle:
        handle.write("".join(out))
    print("wrote demo/shaders.js")
    return 0


def first_difference(a, b):
    left, right = a.splitlines(), b.splitlines()
    for i in range(max(len(left), len(right))):
        x = left[i] if i < len(left) else "<missing>"
        y = right[i] if i < len(right) else "<missing>"
        if x != y:
            return i + 1, x, y
    return None


def check(cpp, js):
    problems = 0

    listed = {symbol for _, symbol in SHADERS}
    for symbol in cpp_pieces(cpp):
        if symbol not in listed:
            print(f"FAIL  {SHADERS_CPP} defines {symbol}, which this check does not list -- "
                  "add it to SHADERS (and to the page, or say on the page why not)")
            problems += 1

    for name, symbol in SHADERS:
        cpp_text, count = from_cpp(cpp, symbol)
        js_text, complaint = from_js(js, name)
        if cpp_text is None:
            print(f"FAIL  {symbol} not found in {SHADERS_CPP} (or not a run of adjacent raw strings)")
            problems += 1
        elif complaint is not None:
            print(f"FAIL  {name} in demo/shaders.js has a {complaint}")
            problems += 1
        elif js_text is None:
            print(f"FAIL  {name} not found in demo/shaders.js")
            problems += 1
        elif cpp_text == js_text:
            joined = f", {count} raw strings joined" if count > 1 else ""
            print(f"ok    {name:<22} matches {symbol} ({len(cpp_text)} chars{joined})")
        else:
            problems += 1
            print(f"FAIL  {name} has drifted from {symbol}")
            where = first_difference(cpp_text, js_text)
            if where:
                print(f"        first difference at line {where[0]}")
                print(f"          C++: {where[1]}")
                print(f"          js : {where[2]}")

    for path, text in ASSEMBLY:
        try:
            body = read(*path.split("/"))
        except FileNotFoundError:
            print(f"FAIL  {path} is missing")
            problems += 1
            continue
        if text in body:
            print(f"ok    assembly: {path} still says {text.splitlines()[0][:60]}")
        else:
            print(f"FAIL  assembly: {path} no longer says: {text.splitlines()[0]}")
            problems += 1
    return problems


def main(argv):
    cpp = read(*SHADERS_CPP.split("/"))
    if "--write" in argv:
        return write(cpp)
    try:
        js = read("demo", "shaders.js")
    except FileNotFoundError:
        print("FAIL  demo/shaders.js is missing -- run demo/tools/check_shaders.py --write")
        return 1
    problems = check(cpp, js)
    print()
    if problems:
        print(f"{problems} problem(s) -- rerun demo/tools/check_shaders.py --write for a drifted copy "
              "(never edit shaders.js by hand); an assembly or unlisted-piece failure needs a reader")
        return 1
    print(f"all {len(SHADERS)} shader pieces and their assembly are the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
