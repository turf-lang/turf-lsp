cmake_minimum_required(VERSION 3.15)
project(turf-lsp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

# Fetch nlohmann/json for JSON-RPC parsing
include(FetchContent)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# Setup includes
include_directories(include)

# Where the compiler checkout is. The default is the layout the README
# documents (turf-lang/ and turf-lsp/ side by side); any other directory name
# is passed as -DTURF_LANG_DIR=<path>.
set(TURF_LANG_DIR "${CMAKE_SOURCE_DIR}/../turf-lang" CACHE PATH "turf-lang (compiler) checkout")

# The compiler revision, for `turf-lsp --version`. Not a build input: a
# checkout that is not a git repository (a source archive) just says unknown.
execute_process(
    COMMAND git -C ${TURF_LANG_DIR} rev-parse --short=12 HEAD
    OUTPUT_VARIABLE TURF_LSP_COMPILER_REV
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _turf_rev_status)
if(NOT _turf_rev_status EQUAL 0 OR TURF_LSP_COMPILER_REV STREQUAL "")
    set(TURF_LSP_COMPILER_REV "unknown")
endif()

# Generate Version.h from the project() version above -- single source of
# truth for both the --version CLI flag and serverInfo.version.
configure_file(${CMAKE_SOURCE_DIR}/include/Version.h.in ${CMAKE_BINARY_DIR}/generated/Version.h @ONLY)
include_directories(${CMAKE_BINARY_DIR}/generated)

# Everything but main(): compiled once, linked into the server and into the
# unit tests (tests/unit_tests.cpp).
add_library(turf_lsp_core OBJECT
    src/LSPHandler.cpp
    src/CompilerService.cpp
    src/SemanticQuery.cpp
    src/Logger.cpp
    src/LSPFramer.cpp
    src/PositionMapper.cpp
    src/DocumentStore.cpp
    src/WorkspaceIndex.cpp
    src/StdlibLocator.cpp
    src/ProjectGraph.cpp
    src/TextFeatures.cpp
)

add_executable(turf-lsp src/main.cpp)
add_executable(turf-lsp-tests tests/unit_tests.cpp)
enable_testing()
add_test(NAME unit COMMAND turf-lsp-tests)

# The language server runs the compiler's front end only, never `--smart`, so
# it does not need the local-model runtime (vendored llama.cpp). Off unless the
# caller turns it on; a cache entry, so an explicit -DTURF_SMART=ON still wins.
set(TURF_SMART OFF CACHE BOOL "Build the --smart model runtime into turfcore")

# Add the compiler as a subdirectory to get the turfcore target.
# EXCLUDE_FROM_ALL: only what turf-lsp links against is built (turfcore), not
# the compiler executable or the runtime library that compiled programs link,
# neither of which the language server uses -- and the runtime library would
# otherwise be written into this source tree's lib/.
add_subdirectory(${TURF_LANG_DIR} turflang_build EXCLUDE_FROM_ALL)

# Link nlohmann_json, turfcore, and the platform thread library (the
# worker/debounce thread split -- doc 01 P0.1 -- needs std::thread's
# underlying implementation linked explicitly on some platforms/toolchains).
target_link_libraries(turf_lsp_core PUBLIC nlohmann_json::nlohmann_json turfcore Threads::Threads)
target_link_libraries(turf-lsp PRIVATE turf_lsp_core)
target_link_libraries(turf-lsp-tests PRIVATE turf_lsp_core)
