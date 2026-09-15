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

# Add source files
set(SOURCES
    src/main.cpp
    src/LSPHandler.cpp
    src/CompilerService.cpp
    src/SemanticQuery.cpp
    src/Logger.cpp
    src/LSPFramer.cpp
    src/PositionMapper.cpp
    src/DocumentStore.cpp
    src/WorkspaceIndex.cpp
)

add_executable(turf-lsp ${SOURCES})

# Add turf-lang subdirectory to get the turfcore target
add_subdirectory(../turf-lang turflang_build)

# Link nlohmann_json, turfcore, and the platform thread library (the
# worker/debounce thread split -- doc 01 P0.1 -- needs std::thread's
# underlying implementation linked explicitly on some platforms/toolchains).
target_link_libraries(turf-lsp PRIVATE nlohmann_json::nlohmann_json turfcore Threads::Threads)
