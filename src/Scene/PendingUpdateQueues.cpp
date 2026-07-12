// PendingUpdateQueues is header-only in practice: the setters are trivial
// enqueue-under-lock one-liners and the withXLocked() drains are templates, so
// their definitions must live in the header.  This TU exists so the queue
// compiles as part of the wpScene library (a self-containment check for the
// header) and so a future non-inline helper has a home without touching CMake.
#include "Scene/PendingUpdateQueues.hpp"
