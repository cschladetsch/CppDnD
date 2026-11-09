cmake_minimum_required(VERSION 3.20)
project(CppDndSim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------------------------
# Shared model store (https://github.com/cschladetsch/CppLmmModelStore): model directory lookup and
# the DeepSeek-style SSE stream parser used by the LLM client. Only the two small sources are
# compiled, against the vendored nlohmann/json, so the library's own CMake (and its extra
# dependencies) is not involved. Use -DDND_MODELSTORE_DIR=<checkout> to build offline.
# ---------------------------------------------------------------------------------------------
option(DND_USE_MODELSTORE "Use CppLmmModelStore for model paths and LLM stream parsing" ON)
set(DND_MODELSTORE_DIR "" CACHE PATH "Local checkout of CppLmmModelStore (default: fetched from GitHub)")

add_library(dnd_json INTERFACE)
target_include_directories(dnd_json SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

if(DND_USE_MODELSTORE)
    if(DND_MODELSTORE_DIR)
        set(modelstore_SOURCE_DIR "${DND_MODELSTORE_DIR}")
    else()
        include(FetchContent)
        FetchContent_Declare(modelstore
            GIT_REPOSITORY https://github.com/cschladetsch/CppLmmModelStore.git
            GIT_TAG master
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR dnd_no_cmake)   # download only; its CMakeLists is intentionally not added
        FetchContent_MakeAvailable(modelstore)
    endif()
    add_library(dnd_modelstore STATIC
        ${modelstore_SOURCE_DIR}/src/ModelStore.cpp
        ${modelstore_SOURCE_DIR}/src/DeepSeekStreamParser.cpp)
    target_include_directories(dnd_modelstore PUBLIC ${modelstore_SOURCE_DIR}/include)
    target_link_libraries(dnd_modelstore PUBLIC dnd_json)
endif()

# ---------------------------------------------------------------------------------------------
add_library(dnd_core STATIC
    src/Dice.cpp src/Types.cpp src/Grid.cpp src/Actor.cpp src/Content.cpp
    src/Combat.cpp src/AI.cpp src/Human.cpp src/DM.cpp src/Json.cpp
    src/Game.cpp src/GameView.cpp src/Llm.cpp src/Agents.cpp src/History.cpp src/Images.cpp src/Scene.cpp)
target_include_directories(dnd_core PUBLIC include)
target_link_libraries(dnd_core PUBLIC dnd_json Threads::Threads)
target_compile_definitions(dnd_core PUBLIC "DND_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\"")
if(DND_USE_MODELSTORE)
    target_link_libraries(dnd_core PUBLIC dnd_modelstore)
    target_compile_definitions(dnd_core PUBLIC DND_HAVE_MODELSTORE)
endif()
if(WIN32)
    target_compile_definitions(dnd_core PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX _CRT_SECURE_NO_WARNINGS)
    if(TARGET dnd_modelstore)
        target_compile_definitions(dnd_modelstore PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    target_link_libraries(dnd_core PUBLIC ws2_32)
endif()

add_executable(dnd_sim src/main.cpp)
target_link_libraries(dnd_sim PRIVATE dnd_core)

add_executable(dnd_server src/server_main.cpp)
target_link_libraries(dnd_server PRIVATE dnd_core)
target_compile_definitions(dnd_server PRIVATE "DND_WEB_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/web\"" "DND_ART_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/art\"" "DND_HISTORY_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/history\"")

enable_testing()

# GoogleTest: an installed copy if there is one, otherwise fetched (needs network on the first configure).
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
    FetchContent_MakeAvailable(googletest)
endif()
include(GoogleTest)

add_executable(dnd_tests
    tests/main.cpp tests/test_core.cpp tests/test_game.cpp tests/test_llm.cpp tests/test_history.cpp tests/test_images.cpp tests/test_scene.cpp tests/test_camera.cpp tests/test_art.cpp)
target_link_libraries(dnd_tests PRIVATE dnd_core GTest::gtest)
if(MSVC)
    target_compile_options(dnd_tests PRIVATE /wd4389 /wd4018)
else()
    target_compile_options(dnd_tests PRIVATE -Wno-sign-compare)   # EXPECT_EQ(container.size(), 3)
endif()
gtest_discover_tests(dnd_tests DISCOVERY_MODE PRE_TEST)   # every TEST() is its own CTest entry

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_test(NAME api_smoke COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/api_smoke.py spawn:$<TARGET_FILE:dnd_server> 4)
endif()

install(TARGETS dnd_sim dnd_server RUNTIME DESTINATION bin)
install(DIRECTORY data web art DESTINATION share/dnd OPTIONAL)
