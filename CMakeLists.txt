cmake_minimum_required(VERSION 3.20)
project(rapture_recomp C)

# Usage:
#   cmake -S . -B build -G Ninja -DGAME_DIR="D:/path/to/MUSHOOT" -DGAME_CONFIG=configs/mushoot.toml
#   cmake --build build && build/test_mushoot_crt build/gen
#
# The recompiler runs at configure/build time on the user's own game files and
# writes the generated C into the build directory (never into the source tree).

set(GAME_CONFIG "${CMAKE_SOURCE_DIR}/configs/mushoot.toml" CACHE FILEPATH "game config")
set(GAME_DIR "" CACHE PATH "folder with the user's original game files")
if(NOT GAME_DIR)
  message(FATAL_ERROR "set -DGAME_DIR to the folder that holds your copy of the game")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")

# Run the recompiler at configure time so the list of generated files is known.
execute_process(
  COMMAND "${Python3_EXECUTABLE}" -m recomp "${GAME_CONFIG}" --game-dir "${GAME_DIR}" --out "${GEN_DIR}"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  RESULT_VARIABLE RECOMP_RESULT)
if(NOT RECOMP_RESULT EQUAL 0)
  message(FATAL_ERROR "recompiler failed")
endif()
file(GLOB GEN_SOURCES "${GEN_DIR}/code_*.c")
file(GLOB RECOMP_PY "${CMAKE_SOURCE_DIR}/recomp/*.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GAME_CONFIG} ${RECOMP_PY})

add_library(recomp_runtime STATIC runtime/runtime.c win32/hle_tables.c)
target_include_directories(recomp_runtime PUBLIC runtime)

add_library(game_gen STATIC ${GEN_SOURCES} "${GEN_DIR}/dispatch.c")
target_include_directories(game_gen PUBLIC "${GEN_DIR}")
target_link_libraries(game_gen PUBLIC recomp_runtime)
# Generated code: strict aliasing is irrelevant (memcpy accessors), warnings are noise.
target_compile_options(game_gen PRIVATE -O1 -w -fno-strict-aliasing)

add_executable(test_mushoot_crt tests/test_mushoot_crt.c)
target_link_libraries(test_mushoot_crt PRIVATE game_gen)

enable_testing()
add_test(NAME mushoot_crt COMMAND test_mushoot_crt "${GEN_DIR}")
