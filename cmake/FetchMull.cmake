# FetchMull.cmake — Download prebuilt Mull binaries matching the current Clang version.
#
# Sets:
#   MULL_PLUGIN_PATH  — path to the mull-ir-frontend shared library
#   MULL_RUNNER       — path to the mull-runner executable
#
# Requires: Clang compiler, internet access at configure time.

set(MULL_VERSION "0.31.1" CACHE STRING "Mull release version to download")

# Detect Clang major version
execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} --version
    OUTPUT_VARIABLE _clang_version_output
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REGEX MATCH "clang version ([0-9]+)" _match "${_clang_version_output}")
set(_clang_major "${CMAKE_MATCH_1}")
if(NOT _clang_major)
    message(FATAL_ERROR "FetchMull: cannot detect Clang major version")
endif()
message(STATUS "FetchMull: Clang major version = ${_clang_major}")

# Detect architecture
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(_mull_arch "aarch64")
else()
    set(_mull_arch "amd64")
endif()

# Download directory (cached across rebuilds)
set(MULL_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_mull" CACHE PATH "Mull download directory")
set(_mull_stamp "${MULL_DOWNLOAD_DIR}/mull-${MULL_VERSION}-${_clang_major}.stamp")

if(EXISTS "${_mull_stamp}")
    message(STATUS "FetchMull: using cached Mull ${MULL_VERSION} for LLVM ${_clang_major}")
else()
    message(STATUS "FetchMull: downloading Mull ${MULL_VERSION} for LLVM ${_clang_major} (${_mull_arch})...")

    # Query GitHub API for release assets matching our Clang major version
    set(_api_url "https://api.github.com/repos/mull-project/mull/releases/tags/${MULL_VERSION}")
    file(DOWNLOAD "${_api_url}" "${MULL_DOWNLOAD_DIR}/release.json"
         STATUS _dl_status
         TIMEOUT 30)
    list(GET _dl_status 0 _dl_code)
    if(NOT _dl_code EQUAL 0)
        message(FATAL_ERROR "FetchMull: failed to query GitHub releases API (${_dl_status})")
    endif()

    # Parse JSON to find matching .deb asset URL
    # Match pattern: Mull-{major}-{version}-LLVM-{major}.x.x-ubuntu-{arch}-*.deb
    file(READ "${MULL_DOWNLOAD_DIR}/release.json" _release_json)
    # Extract all browser_download_url values
    string(REGEX MATCHALL "\"browser_download_url\"[^\"]*\"([^\"]*)\"" _url_matches "${_release_json}")

    set(_deb_url "")
    foreach(_match IN LISTS _url_matches)
        string(REGEX MATCH "\"(https://[^\"]*)\"" _ "${_match}")
        set(_url "${CMAKE_MATCH_1}")
        # Match: Mull-{clang_major}-...-ubuntu-{arch}-*.deb
        if(_url MATCHES "Mull-${_clang_major}-.*-ubuntu-${_mull_arch}-.*\\.deb$")
            set(_deb_url "${_url}")
            break()
        endif()
    endforeach()

    if(NOT _deb_url)
        message(FATAL_ERROR
            "FetchMull: no prebuilt Mull ${MULL_VERSION} found for LLVM ${_clang_major} ${_mull_arch}.\n"
            "Available assets can be checked at: https://github.com/mull-project/mull/releases/tag/${MULL_VERSION}")
    endif()

    message(STATUS "FetchMull: downloading ${_deb_url}")
    get_filename_component(_deb_name "${_deb_url}" NAME)
    set(_deb_path "${MULL_DOWNLOAD_DIR}/${_deb_name}")

    file(DOWNLOAD "${_deb_url}" "${_deb_path}"
         STATUS _dl_status
         SHOW_PROGRESS
         TIMEOUT 120)
    list(GET _dl_status 0 _dl_code)
    if(NOT _dl_code EQUAL 0)
        message(FATAL_ERROR "FetchMull: download failed (${_dl_status})")
    endif()

    # Extract: .deb → ar x → data.tar → tar x
    execute_process(
        COMMAND ar x "${_deb_path}"
        WORKING_DIRECTORY "${MULL_DOWNLOAD_DIR}"
        RESULT_VARIABLE _ar_result
    )
    if(NOT _ar_result EQUAL 0)
        message(FATAL_ERROR "FetchMull: 'ar x' failed on ${_deb_name}")
    endif()

    # Find data.tar (may be data.tar.xz, data.tar.gz, data.tar.zst, or plain data.tar)
    file(GLOB _data_tars "${MULL_DOWNLOAD_DIR}/data.tar*")
    list(GET _data_tars 0 _data_tar)
    execute_process(
        COMMAND tar xf "${_data_tar}"
        WORKING_DIRECTORY "${MULL_DOWNLOAD_DIR}"
        RESULT_VARIABLE _tar_result
    )
    if(NOT _tar_result EQUAL 0)
        message(FATAL_ERROR "FetchMull: tar extraction failed")
    endif()

    # Make runner executable
    file(GLOB _runners "${MULL_DOWNLOAD_DIR}/usr/bin/mull-runner-*")
    foreach(_r IN LISTS _runners)
        file(CHMOD "${_r}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
             GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    endforeach()

    # Write stamp
    file(WRITE "${_mull_stamp}" "${_deb_url}\n")
    message(STATUS "FetchMull: extracted successfully")
endif()

# Set output variables
file(GLOB MULL_PLUGIN_PATH "${MULL_DOWNLOAD_DIR}/usr/lib/mull-ir-frontend-${_clang_major}")
file(GLOB MULL_RUNNER "${MULL_DOWNLOAD_DIR}/usr/bin/mull-runner-${_clang_major}")

if(NOT MULL_PLUGIN_PATH)
    message(FATAL_ERROR "FetchMull: mull-ir-frontend-${_clang_major} not found after extraction")
endif()
if(NOT MULL_RUNNER)
    message(FATAL_ERROR "FetchMull: mull-runner-${_clang_major} not found after extraction")
endif()

message(STATUS "FetchMull: plugin = ${MULL_PLUGIN_PATH}")
message(STATUS "FetchMull: runner = ${MULL_RUNNER}")
