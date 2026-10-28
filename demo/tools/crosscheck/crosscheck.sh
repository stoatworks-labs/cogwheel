#!/usr/bin/env bash
#
# The browser demo's port of the CPU half, against the plugin's own C++.
#
#     bash demo/tools/crosscheck/crosscheck.sh
#
# printer.cpp runs scenario.txt on the plugin's machine (Gears, Wheels, Crank,
# Pens, Clock), Controls.cpp's Resolve(), Presets.h and the real CogwheelPlugin
# class's display strings; run.mjs runs it on demo/plugin.js's PORT block. Both
# print every pen step, run, crank state, mapping and display, floats as bit
# patterns, and the two printouts must be identical, byte for byte.
#
# Two details make "identical" possible, and both are about the comparison
# rather than the plugin:
#
#   * printer is built -ffp-contract=off. With contraction on, clang fuses
#     `a * b + c` into one fused multiply-add on arm64, which rounds once where
#     the source (and JavaScript) rounds twice.
#   * The plugin's own powf and logf results are recorded (libm_record.c,
#     inserted with DYLD_INSERT_LIBRARIES) and replayed into the port, which
#     otherwise has only V8's Math. The line count that differs WITHOUT the
#     replay is printed too: that is what the page itself computes.
#
# Needs clang, node and the FFGL submodule; no GL context. About a minute and a
# half, most of it the four mutant runs.
set -euo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
REPO="$( cd "$HERE/../../.." && pwd )"
WORK="$( mktemp -d )"
trap 'rm -rf "$WORK"' EXIT

cd "$REPO"

[[ -f external/ffgl/source/lib/FFGLSDK.cpp ]] \
	|| { echo "FFGL SDK missing -- run: git submodule update --init --recursive"; exit 1; }

version="$( sed -n 's/^project(Cogwheel VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt | head -1 )"

clang -dynamiclib -O2 "$HERE/libm_record.c" -o "$WORK/libm_record.dylib"

# Every source file of cogwheel_core, as CMakeLists.txt lists it, and the SDK.
clang++ -std=c++17 -O2 -ffp-contract=off -w \
	-DGL_SILENCE_DEPRECATION -DCOGWHEEL_VERSION="\"$version\"" \
	-Isource -Iexternal/ffgl/source/lib \
	"$HERE/printer.cpp" \
	source/Cogwheel.cpp source/Config.cpp source/Controls.cpp source/Diag.cpp \
	source/machine/Clock.cpp source/machine/Crank.cpp source/machine/Gears.cpp \
	source/machine/Pens.cpp source/machine/Wheels.cpp \
	source/render/PaperBuffer.cpp source/render/Shaders.cpp source/render/Sheet.cpp \
	external/ffgl/source/lib/FFGLSDK.cpp \
	-framework OpenGL -framework AppKit \
	-o "$WORK/printer"

LIBM_RECORD="$WORK/libm.txt" DYLD_INSERT_LIBRARIES="$WORK/libm_record.dylib" \
	"$WORK/printer" "$HERE/scenario.txt" > "$WORK/cpp.txt"
recorded="$( wc -l < "$WORK/libm.txt" | tr -d ' ' )"
[[ "$recorded" -gt 0 ]] || { echo "FAIL  no libm calls were recorded -- the interposition did not take"; exit 1; }

node "$HERE/run.mjs" "$HERE/scenario.txt" "$WORK/libm.txt" > "$WORK/js.txt" 2> "$WORK/replay.txt"
node "$HERE/run.mjs" "$HERE/scenario.txt" > "$WORK/js-own.txt"

lines="$( wc -l < "$WORK/cpp.txt" | tr -d ' ' )"
steps="$( grep -c '^S ' "$WORK/cpp.txt" || true )"
shown="$( grep -c '^D ' "$WORK/cpp.txt" || true )"
own="$( { diff "$WORK/cpp.txt" "$WORK/js-own.txt" || true; } | grep -c '^<' || true )"

if ! cmp -s "$WORK/cpp.txt" "$WORK/js.txt"; then
	echo "FAIL  the port's printout differs from the plugin's"
	diff "$WORK/cpp.txt" "$WORK/js.txt" | head -8 | cut -c1-240
	exit 1
fi
echo "ok    all $lines lines identical: $steps pen steps, $shown display sweeps, every mapping"
echo "      ($( cat "$WORK/replay.txt" ))"
echo "      without the replay, on V8's own Math: $own line(s) differ"

# The mutants: a port wrong by a millionth of a tooth, by one float rounding,
# by one bit of the hash, by one decimal place of a display. Each must FAIL,
# or the match above proves nothing.
mutants="$( sed -n '/^const MUTANTS/,/^];/p' "$HERE/run.mjs" | grep -c "^  \['" || true )"
caught=0
for (( n = 0; n < mutants; n++ )); do
	status=0
	MUTANT=$n node "$HERE/run.mjs" "$HERE/scenario.txt" "$WORK/libm.txt" > "$WORK/mutant.txt" 2>/dev/null || status=$?
	if [[ "$status" -eq 3 ]]; then
		echo "FAIL  mutant $n no longer matches plugin.js -- update MUTANTS in run.mjs"
		exit 1
	fi
	if cmp -s "$WORK/cpp.txt" "$WORK/mutant.txt"; then
		echo "FAIL  mutant $n passed: $( sed -n "/^const MUTANTS/,/^];/p" "$HERE/run.mjs" | grep "^  \['" | sed -n "$(( n + 1 ))p" | cut -c1-100 )"
		exit 1
	fi
	caught=$(( caught + 1 ))
done
[[ "$caught" -gt 0 ]] || { echo "FAIL  no mutants were run"; exit 1; }
echo "ok    all $caught mutants caught"
