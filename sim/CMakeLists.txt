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

add_library(dnd_core STATIC
    src/Dice.cpp src/Types.cpp src/Grid.cpp src/Actor.cpp src/Content.cpp
    src/Combat.cpp src/AI.cpp src/Human.cpp src/DM.cpp src/Session.cpp)
target_include_directories(dnd_core PUBLIC include)

add_executable(dnd_sim src/main.cpp)
target_link_libraries(dnd_sim PRIVATE dnd_core)

enable_testing()
add_executable(dnd_tests tests/tests.cpp)
target_link_libraries(dnd_tests PRIVATE dnd_core)
add_test(NAME dnd_tests COMMAND dnd_tests)
