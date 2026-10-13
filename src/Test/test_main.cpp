#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include "Utils/Logging.h"

int main(int argc, char** argv) {
    // LOG_INFO is off by default in production (WEKDE_LOG_INFO turns it on),
    // but many suites assert on INFO diagnostics through the log sink, e.g.
    // the remap operators naming a value they cannot serve.  The gate itself
    // is pinned in test_Logging.cpp, which sets it explicitly per case.
    wallpaper::SetLogInfoEnabled(true);
    return doctest::Context(argc, argv).run();
}
