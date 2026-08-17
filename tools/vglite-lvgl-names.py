#!/usr/bin/env python3
"""Names LVGL's VG_LITE backend references that the vendored VGLite driver lacks.

    tools/vglite-lvgl-names.py                     # list them, sorted
    tools/vglite-lvgl-names.py --check <header>    # gate: 0 = covered, 1 = drift

WHY THIS EXISTS. LVGL 9.4's VG_LITE backend targets a NEWER NXP driver than the
one vendored in the VGLite sibling repo (VGLITE_HEADER_VERSION 6). It references
47 gcFEATURE_BIT_VG_* names where the driver defines 9, plus 9 error/format
names. lv_vg_lite_feature_string() and lv_vg_lite_error_string() are compiled
unconditionally, so every name must EXIST at compile time even though most are
only ever used for logging. A compatibility header supplies the difference.

That difference is a moving target: bumping either pin changes it silently. The
--check mode is therefore a gate, not a convenience. Its failure mode when
absent is nasty -- a name that disappears from the driver and is not in the
header is a compile error (fine), but a name that MOVES from shim to driver
would leave the shim's #define shadowing a real enumerator, which compiles and
reads the wrong feature.

★ REGEX ARTIFACT, do not "fix" by widening the pattern. LVGL builds its case
labels with token pasting:

    #define FEATURE_ENUM_TO_STRING(e)  case (gcFEATURE_BIT_VG_##e): return #e

Scanning text before the preprocessor runs, `gcFEATURE_BIT_VG_##e` matches as
the literal name `gcFEATURE_BIT_VG_e`. It is not a symbol. It is discarded
below by name; a first draft of this script emitted it and the shim would have
grown a bogus entry.
"""
import glob
import os
import re
import sys

LIB = os.environ.get("TEENSY_LIB_ROOT", os.path.expanduser("~/Development"))
BACKEND = os.path.join(LIB, "LVGL", "lvgl", "src", "draw", "vg_lite")
# Every header a consumer sees via `#include <vg_lite.h>`. If the port grows
# another public header, add it here or its names look "missing".
HEADERS = [
    os.path.join(LIB, "VGLite", "inc", "vg_lite.h"),
    os.path.join(LIB, "VGLite", "inc", "vg_lite_hal.h"),
]

FEATURE_RE = re.compile(r"\bgcFEATURE_BIT_VG_\w+")
FEATURE_MACRO_RE = re.compile(r"FEATURE_ENUM_TO_STRING\((\w+)\)")
ENUM_MACRO_RE = re.compile(r"VG_LITE_ENUM_TO_STRING\((\w+)\)")
ANY_VG_LITE_RE = re.compile(r"\bVG_LITE_\w+")

PASTE_ARTIFACTS = {"gcFEATURE_BIT_VG_e", "VG_LITE_e"}


def _backend_sources():
    if not os.path.isdir(BACKEND):
        sys.stderr.write(
            "vglite-lvgl-names: no LVGL VG_LITE backend at %s\n"
            "  (set TEENSY_LIB_ROOT, or check the LVGL checkout)\n" % BACKEND)
        sys.exit(2)
    return sorted(glob.glob(os.path.join(BACKEND, "*.c")) +
                  glob.glob(os.path.join(BACKEND, "*.h")))


def missing():
    """(feature_names, enum_names) referenced by LVGL, absent from the driver."""
    need_feat, need_enum = set(), set()
    for path in _backend_sources():
        src = open(path).read()
        need_feat |= set(FEATURE_RE.findall(src))
        need_feat |= set("gcFEATURE_BIT_VG_" + m for m in FEATURE_MACRO_RE.findall(src))
        need_enum |= set("VG_LITE_" + m for m in ENUM_MACRO_RE.findall(src))

    have = set()
    for path in HEADERS:
        if not os.path.exists(path):
            sys.stderr.write("vglite-lvgl-names: missing driver header %s\n" % path)
            sys.exit(2)
        src = open(path).read()
        have |= set(FEATURE_RE.findall(src))
        have |= set(ANY_VG_LITE_RE.findall(src))

    feat = sorted(need_feat - have - PASTE_ARTIFACTS)
    enum = sorted(need_enum - have - PASTE_ARTIFACTS)
    return feat, enum


def main(argv):
    feat, enum = missing()

    if "--check" in argv:
        header = argv[argv.index("--check") + 1]
        text = open(header).read()
        gaps = [n for n in feat + enum
                if not re.search(r"\b%s\b" % re.escape(n), text)]
        if gaps:
            print("VGLITE-COMPAT DRIFT: %d name(s) referenced by LVGL and defined "
                  "by neither the driver nor %s:" % (len(gaps), header))
            for n in gaps:
                print("   ", n)
            print("Add them to the compat header. ★ If any is a PIXEL FORMAT, read")
            print("docs/superpowers/specs/2026-08-17-vglite-phase2-design.md §5")
            print("first: formats are not inert, they are handed to the GPU.")
            return 1
        print("VGLITE-COMPAT: OK (%d feature + %d enum names shimmed)"
              % (len(feat), len(enum)))
        return 0

    for n in feat + enum:
        print(n)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
