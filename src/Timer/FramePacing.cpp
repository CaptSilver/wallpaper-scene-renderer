// FramePacing is header-only: constexpr integer math has to live in the
// header for the tests and FrameTimer to constant-fold it.  This TU exists so
// the header compiles standalone as part of wpTimer (self-containment check)
// and so a future non-inline helper has a home without touching CMake.
#include "Timer/FramePacing.hpp"
