// STRATA's entry point, and nothing else.
//
// `ENAGA_APP(Strata)` defines the package's entry points (`enaga_app_main`), which belong to a
// device build: `host/Makefile` cannot compile this translation unit at all. So this file holds
// that one line, and the App class it names lives in app/app.hpp, where the host suite can
// include it -- `tests/test_ui.cpp` drives the same class the unit runs. See app/app.hpp.
#include "app.hpp"

ENAGA_APP(Strata)
