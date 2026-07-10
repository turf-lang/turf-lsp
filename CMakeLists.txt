cmake_minimum_required(VERSION 3.15)
project(turf-lsp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
)

add_executable(turf-lsp ${SOURCES})

# Link nlohmann_json
target_link_libraries(turf-lsp PRIVATE nlohmann_json::nlohmann_json)

# Later: link against turflang libraries here
# target_link_libraries(turf-lsp PRIVATE turflang_core)
