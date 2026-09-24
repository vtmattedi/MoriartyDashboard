"""Apply C++-only warning flags.

LVGL 8's style selector is by design a bitwise OR of a part and a state
(LV_PART_MAIN | LV_STATE_PRESSED). C++20 deprecated that for unrelated enums,
and there is no other way to call the API, so the warning is pure noise that
would bury real ones.

It has to be set here rather than in platformio.ini's build_flags: those are
passed to the C compiler too, and LVGL itself is C, so GCC would then warn once
per C file that the option does not apply to C.
"""
Import("env")

env.Append(CXXFLAGS=["-Wno-deprecated-enum-enum-conversion"])
