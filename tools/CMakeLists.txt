add_executable(wp-pkg wp_pkg.cpp)
target_compile_features(wp-pkg PRIVATE cxx_std_20)

# nlohmann_json is already a project submodule and header-only (INTERFACE
# target) — linking it costs nothing and lets the `scripts` subcommand
# walk scene.json without a hand-rolled JSON parser.
target_link_libraries(wp-pkg PRIVATE nlohmann_json)

if(WIN32)
    # only needed if we ever build on Win — no-op on Linux
    target_compile_definitions(wp-pkg PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

install(TARGETS wp-pkg RUNTIME DESTINATION bin OPTIONAL)
