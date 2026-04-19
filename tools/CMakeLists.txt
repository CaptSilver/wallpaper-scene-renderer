add_executable(wp-pkg wp_pkg.cpp)
target_compile_features(wp-pkg PRIVATE cxx_std_20)

if(WIN32)
    # only needed if we ever build on Win — no-op on Linux
    target_compile_definitions(wp-pkg PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

install(TARGETS wp-pkg RUNTIME DESTINATION bin OPTIONAL)
