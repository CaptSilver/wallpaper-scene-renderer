set(LIB_NAME wpFs)

add_library(${LIB_NAME} INTERFACE)

target_include_directories(${LIB_NAME} INTERFACE include)
# No compiled TUs to instrument (header-only) -- wek_apply_mull_instrumentation
# no-ops on an INTERFACE library, called anyway so this stays instrumented the
# moment it ever grows a source file.
wek_apply_mull_instrumentation(${LIB_NAME})
