#!/usr/bin/env bash
#
# Everything that can be checked without a human, in one command.
#
#     tools/verify.sh
#
# ---------------------------------------------------------------- the point
#
# Half of this file checks things the RELEASE job checks. That is deliberate,
# and it is the fleet's most expensive lesson: a check that only ever runs in
# CI, after a tag, is a check that will catch you after the tag. The bundle
# layout, the plist, the architectures and the signature can all be verified
# here in a second, and the alternative is a failed release and a force-moved
# tag.
#
# The two that have actually bitten this fleet, both while starting a new plugin
# repo by copying an old one -- which is exactly how this repo started:
#
#   * `CFBundleExecutable` carrying the PREVIOUS plugin's name, because
#     cmake/InfoOFX.plist.in was copied unchanged. Nothing fails: the bundle
#     assembles, the binary is universal, `nm` finds the entry point and a probe
#     renders a correct frame. Then codesign says "code object is not signed at
#     all" and mentions nothing about a plist.
#
#   * A macOS build that is quietly arm64-only, because CMAKE_OSX_ARCHITECTURES
#     was set after the first target existed. The build log calls that a
#     success. Only `lipo` knows.
#
set -uo pipefail

cd "$(dirname "$0")/.."

PASS=0
FAIL=0

ok()    { printf '  \033[32mok\033[0m    %s\n' "$1"; PASS=$((PASS+1)); }
bad()   { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; FAIL=$((FAIL+1)); }
head_() { printf '\n\033[1m%s\033[0m\n' "$1"; }

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the plugin does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
    local dir bad=0 n=0 shader

    if ! command -v glslc >/dev/null 2>&1; then
        printf '   skipped: glslc not installed (brew install shaderc)\n'
        return 0
    fi

    dir="$( mktemp -d )"

    python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL. One file; if that stops being true, add to
# this list rather than letting the extraction quietly cover less of it.
FILES = [ "source/render/Shaders.cpp" ]

# Shaders the plugin assembles at run time. Mirrors vertexSource() and
# fragmentSource() in Shaders.cpp and their call sites -- a name that has moved
# is a KeyError below, not a silent skip.
ASSEMBLED = {
	"screenVertex":  [ "kConstants", "kScreenVertexBody" ],
	"inkVertex":     [ "kConstants", "kInkVertexBody" ],
	"inkFragment":   [ "kConstants", "kFragmentHelpers", "kInkFragmentBody" ],
	"fadeFragment":  [ "kConstants", "kFragmentHelpers", "kFadeFragmentBody" ],
	"sheetFragment": [ "kConstants", "kFragmentHelpers", "kSheetFragmentBody" ],
}

named, unnamed = {}, []
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(?:(\w+)\s*(?:\[\s*\])?\s*=\s*)?R"\((.*?)\)"', text, re.S ):
		if m.group( 1 ): named[ m.group( 1 ) ] = m.group( 2 )
		else:            unnamed.append( m.group( 2 ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

def piece( p ):
	if isinstance( p, int ):       return unnamed[ p ]
	if p.startswith( "#version" ): return p
	return named[ p ]

for name, parts in ASSEMBLED.items():
	emit( name, "".join( piece( p ) for p in parts ) )
SHADERS_PY

    for shader in "$dir"/*.vert "$dir"/*.frag; do
        [ -e "$shader" ] || continue
        n=$(( n + 1 ))
        if ! glslc --target-env=opengl4.5 -fauto-map-locations \
               "$shader" -o /dev/null 2>"$dir/err"; then
            printf '   %s does not compile\n' "$( basename "$shader" )"
            sed "s|$dir/||; s|^|      |" "$dir/err"
            bad=$(( bad + 1 ))
        fi
    done

    if [ "$n" -eq 0 ]; then
        # No shaders at all is a FAILURE, not a pass. It means the extraction
        # above has lost track of where this repo keeps its GLSL, and a check
        # that silently looks at nothing is worse than no check.
        printf '   no shaders were extracted -- the extraction has gone stale\n'
        rm -rf "$dir"
        return 1
    fi

    if [ "$bad" -eq 0 ]; then
        printf '   %d shaders, all compile\n' "$n"
    fi
    rm -rf "$dir"
    return "$bad"
}

#---------------------------------------------------------------------------
head_ "Shaders"
#---------------------------------------------------------------------------
if shaders_compile; then
    ok "every shader compiles"
else
    bad "a shader does not compile"
fi

# ---------------------------------------------------------------------------
head_ "Build (universal, both plugins, OFX, harness)"
# ---------------------------------------------------------------------------
# The build directory is DELETED first, and that is not belt and braces.
#
# `cmake -B build` on an existing tree re-uses the cache, and the cache is
# exactly where the architecture list and the BUILD_OFX switch live. Anyone who
# configured once with `-DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_OFX=OFF` for a
# fast iteration loop -- which is the documented way to work in CLAUDE.md --
# leaves a tree where this script happily rebuilds, finds a single-architecture
# binary and no OFX bundle, and reports both as defects in the source.
rm -rf build

if cmake -B build -DCMAKE_BUILD_TYPE=Release >/tmp/cogwheel-configure.log 2>&1 \
   && cmake --build build -j8 >/tmp/cogwheel-build.log 2>&1; then
    ok "configured and built"
else
    bad "build failed -- see /tmp/cogwheel-build.log"
    tail -25 /tmp/cogwheel-build.log
    exit 1
fi

SRC_BIN="build/Cogwheel.bundle/Contents/MacOS/Cogwheel"
FX_BIN="build/Cogwheel Ink.bundle/Contents/MacOS/Cogwheel Ink"
OFX_BUNDLE="build/Cogwheel.ofx.bundle"
OFX_BIN="$OFX_BUNDLE/Contents/MacOS/Cogwheel.ofx"

# ---------------------------------------------------------------------------
head_ "Architectures"
# ---------------------------------------------------------------------------
# lipo, never the build log. A single-architecture build is a successful build.
for binary in "$SRC_BIN" "$FX_BIN" "$OFX_BIN"; do
    if [ ! -f "$binary" ]; then
        bad "missing: $binary"
        continue
    fi
    archs="$(lipo -archs "$binary" 2>/dev/null)"
    case "$archs" in
        *x86_64*arm64*|*arm64*x86_64*) ok "universal: $(basename "$binary") ($archs)" ;;
        *) bad "NOT universal: $(basename "$binary") ($archs)" ;;
    esac
done

# ---------------------------------------------------------------------------
head_ "Entry points"
# ---------------------------------------------------------------------------
# An OBJECT library keeps the file-scope CFFGLPluginInfo alive. In a STATIC
# archive the linker may drop it, giving a bundle that loads, exports plugMain,
# and reports that it contains no plugins -- so exporting the symbol is
# necessary and not sufficient. The host load is what settles it.
#
# The symbol table is captured and matched with `case`, so there is NO PIPELINE
# at any point. `nm -gU "$bin" | grep -q _plugMain` under `set -o pipefail`
# fails BECAUSE the symbol was found: grep -q exits at the first match, nm takes
# SIGPIPE, and pipefail propagates it. It is a race against how fast the
# producer finishes, so it bites the large OFX bundle while the identical line
# against the two smaller FFGL bundles passes, in the same run.
symbols_of() {
    nm -gU "$1" 2>/dev/null || true
}

for binary in "$SRC_BIN" "$FX_BIN"; do
    case "$(symbols_of "$binary")" in
        *_plugMain*) ok "exports plugMain: $(basename "$binary")" ;;
        *) bad "no plugMain: $(basename "$binary")" ;;
    esac
done

case "$(symbols_of "$OFX_BIN")" in
    *_OfxGetPlugin*) ok "exports OfxGetPlugin" ;;
    *) bad "no OfxGetPlugin in the OFX bundle" ;;
esac

# ---------------------------------------------------------------------------
head_ "OFX bundle layout and signing"
# ---------------------------------------------------------------------------
# This whole section exists because it went wrong once, in another repo, and
# only at release time. See the header.
plist_exec="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' \
              "$OFX_BUNDLE/Contents/Info.plist" 2>/dev/null)"
if [ "$plist_exec" = "Cogwheel.ofx" ]; then
    ok "CFBundleExecutable is Cogwheel.ofx"
else
    bad "CFBundleExecutable is '$plist_exec', not Cogwheel.ofx"
fi

if [ -f "$OFX_BUNDLE/Contents/MacOS/$plist_exec" ]; then
    ok "CFBundleExecutable matches a real binary on disk"
else
    bad "CFBundleExecutable names a file that is not there"
fi

# The exact command the release job runs, on a COPY, where it costs a second
# instead of a failed tag.
signdir="$(mktemp -d)"
cp -R "$OFX_BUNDLE" "$signdir/" 2>/dev/null
if codesign --force --sign - --timestamp=none "$signdir/$(basename "$OFX_BUNDLE")" >/dev/null 2>&1; then
    ok "ad-hoc codesign of the OFX bundle succeeds"
else
    bad "codesign fails -- this is the failure that never mentions the plist"
fi
rm -rf "$signdir"

# ---------------------------------------------------------------------------
head_ "The four-character plugin IDs"
# ---------------------------------------------------------------------------
# FFGL identifies a plugin by four characters and a host keys its saved
# compositions on them. Two plugins in one Resolume install sharing an ID is a
# composition that loads the wrong effect, so these must be unique across the
# whole fleet -- CG01 was already cartridge's when this repo started.
ids="$(grep -ohE '"[A-Z0-9]{4}",[[:space:]]*//[[:space:]]*Plugin unique ID' \
       source/SourcePlugin.cpp source/EffectPlugin.cpp 2>/dev/null \
       | grep -oE '"[A-Z0-9]{4}"' | tr -d '"' | sort)"
if [ "$ids" = "$(printf 'CW01\nCW02')" ]; then
    ok "plugin IDs are CW01 and CW02"
else
    bad "plugin IDs are '$(echo "$ids" | tr '\n' ' ')', expected CW01 and CW02"
fi

# ---------------------------------------------------------------------------
head_ "Parameter names"
# ---------------------------------------------------------------------------
# FFGL truncates a name at sixteen characters, in the host, silently. Anything
# over is a control whose label an operator cannot read.
if ./build/cgtest --names >/tmp/cogwheel-names.log 2>&1; then
    ok "every parameter name fits FFGL's 16 characters"
else
    bad "a parameter name is too long -- see /tmp/cogwheel-names.log"
    cat /tmp/cogwheel-names.log
fi

# ---------------------------------------------------------------------------
head_ "The invariants"
# ---------------------------------------------------------------------------
# These are the point of the harness: they turn "ink is subtractive" and "a
# closed figure stops drawing" from sentences in AGENTS.md into things a machine
# checks.
if [ -x build/cgtest ]; then
    for test in closure detail rate beer liveness presets defaults hosts scale guard; do
        log="/tmp/cogwheel-$test.log"
        if ./build/cgtest "--$test" >"$log" 2>&1; then
            ok "cgtest --$test"
        else
            bad "cgtest --$test -- see $log"
            tail -14 "$log"
        fi
    done
else
    bad "cgtest was not built"
fi

# ---------------------------------------------------------------------------
head_ "Controls"
# ---------------------------------------------------------------------------
# A GLSL uniform name that does not match the C++ is silently ignored --
# glGetUniformLocation returns -1 and glUniform(-1) is a documented no-op -- so
# a control can be stone dead while everything compiles, links, loads and
# renders. Nothing else catches it. It found Centre Y on the first run.
if python3 tools/sweep.py >/tmp/cogwheel-sweep.log 2>&1; then
    ok "every parameter changes the drawing"
else
    bad "dead controls -- see /tmp/cogwheel-sweep.log"
    tail -20 /tmp/cogwheel-sweep.log
fi

# ---------------------------------------------------------------------------
head_ "Presets"
# ---------------------------------------------------------------------------
# A preset row with too FEW values is aggregate-initialised to zero and compiles
# without a word, so the row silently means something else. The static_assert in
# Cogwheel.cpp catches a wrong row *count*; only this catches a short row, and a
# fractional value in a column that holds a tooth count.
if python3 tools/check_presets.py >/tmp/cogwheel-presets.log 2>&1; then
    ok "every preset row is the full width, with whole numbers where it matters"
else
    bad "a preset row is wrong -- see /tmp/cogwheel-presets.log"
    cat /tmp/cogwheel-presets.log
fi

# ---------------------------------------------------------------------------
printf '\n\033[1m%d passed, %d failed\033[0m\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
