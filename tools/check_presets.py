#!/usr/bin/env python3
"""Every preset row must carry a full set of values, of the right kind.

Two checks, and the second one is the fleet's expensive lesson.

`Cogwheel.cpp` already static_asserts that the preset *parameter list* and
`presets::kParamCount` agree. What no compiler catches is a preset **row** with
too few values: C++ aggregate initialisation fills the rest with zero, without a
word, so the row is accepted and silently means something else -- and because
the missing values land at the *end* of the row, the symptom is that the framing
of one preset is wrong while every other preset is fine.

A `FF_TYPE_INTEGER` parameter holds the **integer**, not a 0..1 fraction. The
SDK's clamp into 0..1 is guarded by `if( pType == FF_TYPE_STANDARD )`, which is
exactly why the tooth counts are declared as integers in the first place. Write
0.2 into one and it does not mean "a fifth of the way up": it rounds to 0 and
clamps to the range's floor. In this plugin that is not a subtle difference --
a Ring of 12 teeth and a Wheel of 3 is a different machine, not a differently
tuned one.

Both are fifteen-minute bugs to find by eye and one-second ones to find here.
"""

import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent.parent
SOURCE = HERE / "source" / "Presets.h"


def main() -> int:
    text = SOURCE.read_text()

    start = text.index("enum Param\n{")
    end = text.index("kParamCount", start)
    names = [
        line.strip().rstrip(",")
        for line in text[start:end].splitlines()
        if line.strip().startswith("k")
    ]
    expected = len(names)

    if expected == 0:
        print("could not parse the Param enum out of Presets.h", file=sys.stderr)
        return 1

    # Columns whose host parameter is FF_TYPE_INTEGER, with the range declared
    # for it in Cogwheel.cpp. A value here must be a whole number inside that
    # range -- see the module docstring for what happens when it is not.
    integer_columns = {
        "kRing": (12, 360),
        "kWheel": (3, 240),
        "kSkipTeeth": (1, 12),
        "kLayers": (1, 16),
    }

    # Columns whose host parameter is an option or a boolean. Those hold an
    # element value, so a fraction there is the same units bug wearing a
    # different hat -- and a boolean at 0.35 is the one that has actually bitten
    # this fleet, because the FFGL and OFX builds disagreed about it (one read
    # 0.35, the other read false).
    discrete_columns = {
        "kMesh": (0, 1),
        "kSnapSet": (0, 1),
        "kSnapHoles": (0, 1),
        "kDetail": (0, 2),
        "kChange": (0, 3),
        "kWipe": (0, 1),
        "kPenSet": (0, 4),
        "kPenType": (0, 1),
        "kPrint": (0, 1),
    }

    for column in list(integer_columns) + list(discrete_columns):
        if column not in names:
            print(f"the Param enum has no {column}; this check needs updating", file=sys.stderr)
            return 1

    column_of = {name: i for i, name in enumerate(names)}

    failures = 0
    rows = 0

    for match in re.finditer(r'\{\s*"([^"]+)",\s*\{(.*?)\}\s*\}', text, re.S):
        name, body = match.group(1), match.group(2)
        rows += 1

        # Strip trailing line comments before splitting -- the table carries a
        # column-header comment that is full of commas. The cartridge macro is
        # three values and is expanded here so the count comes out right.
        cleaned = "\n".join(line.split("//")[0] for line in body.splitlines())
        cleaned = cleaned.replace("COGWHEEL_CARTRIDGE", "0.0f, 0.0f, 0.0f")
        values = [v for v in cleaned.split(",") if v.strip()]

        if len(values) != expected:
            print(f"FAIL  {name}: {len(values)} values, expected {expected}")
            failures += 1
            continue

        for column, (low, high) in {**integer_columns, **discrete_columns}.items():
            raw = values[column_of[column]].strip().rstrip("f")
            try:
                number = float(raw)
            except ValueError:
                print(f"FAIL  {name}: {column} is {raw!r}, which is not a number")
                failures += 1
                continue

            if number != int(number):
                print(
                    f"FAIL  {name}: {column} = {raw} is a fraction. That parameter is "
                    f"discrete, so it rounds to {int(round(number))} and clamps to {low}."
                )
                failures += 1
            elif not low <= number <= high:
                print(f"FAIL  {name}: {column} = {raw} is outside its range {low}..{high}")
                failures += 1

    if rows == 0:
        print("no preset rows found -- has the table's shape changed?", file=sys.stderr)
        return 1

    if failures:
        print(f"\n{failures} problem(s) across {rows} preset rows")
        return 1

    print(f"ok    {rows} preset rows, {expected} values each")
    return 0


if __name__ == "__main__":
    sys.exit(main())
