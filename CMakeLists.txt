cmake_minimum_required(VERSION 3.24)
project(CppAiDungeon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler options
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

include(FetchContent)

# ------------------------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------------------------

# 1. nlohmann_json
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# 2. cpp-httplib
FetchContent_Declare(
    httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.15.3.tar.gz
)
FetchContent_MakeAvailable(httplib)

# 3. pboettch/json-schema-validator (Required for runtime schema validation)
FetchContent_Declare(
    json-schema-validator
    GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
    GIT_TAG        v2.3.0
)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json-schema-validator)

# 4. GoogleTest (Offline test suite runner)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ------------------------------------------------------------------------------
# Submodule Dependency: cppcoder_core
# ------------------------------------------------------------------------------

# Included via external/cppcoder submodule without touching upstream repo
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/cppcoder/CMakeLists.txt")
    add_subdirectory(external/cppcoder EXCLUDE_FROM_ALL)
else()
    message(FATAL_ERROR "Missing external/cppcoder submodule. Run 'git submodule update --init --recursive'")
endif()

# ------------------------------------------------------------------------------
# Standalone Target: dungeon_core (Static Library)
# ------------------------------------------------------------------------------

add_library(dungeon_core STATIC
    src/Types.cpp
    src/Action.cpp
    src/GameState.cpp
    src/RulesEngine.cpp
    src/Rng.cpp
    src/PerceptionView.cpp
    src/CharacterBuilder.cpp
    src/ModuleLoader.cpp
    src/PartyAgent.cpp
    src/DmNarrator.cpp
    src/DmArbiter.cpp
    src/GameSession.cpp
    src/GameServer.cpp
)

target_include_directories(dungeon_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(dungeon_core PUBLIC
    cppcoder_core
    nlohmann_json_schema_validator
)

# Copy asset assets, schemas, and prompt templates into build output folder
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/prompts DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/schemas DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/data DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

# ------------------------------------------------------------------------------
# Standalone Game Executables
# ------------------------------------------------------------------------------

# Main runtime engine & server
add_executable(dungeon src/main_play.cpp)
target_link_libraries(dungeon PRIVATE dungeon_core)

# Offline module & adventure content generator
add_executable(dungeon-gen src/main_gen.cpp)
target_link_libraries(dungeon-gen PRIVATE dungeon_core)

# Model benchmark & prompt evaluation suite
add_executable(dungeon-eval src/main_eval.cpp)
target_link_libraries(dungeon-eval PRIVATE dungeon_core)

# ------------------------------------------------------------------------------
# Offline CTest Suite
# ------------------------------------------------------------------------------

enable_testing()

add_executable(dungeon_tests
    tests/RulesTests.cpp
    tests/SchemaTests.cpp
    tests/ParseTests.cpp
    tests/ModuleValidatorTests.cpp
    tests/ReplayTests.cpp
    tests/PropertyTests.cpp
)

target_link_libraries(dungeon_tests PRIVATE
    dungeon_core
    gtest_main
)

include(GoogleTest)
gtest_discover_tests(dungeon_tests PROPERTIES LABELS "offline")

