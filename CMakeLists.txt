cmake_minimum_required(VERSION 3.20)
project(rapture_recomp C CXX)

# Usage:
#   cmake -S . -B build -G Ninja -DGAME_DIR="D:/path/to/MUSHOOT" -DGAME_CONFIG=configs/mushoot.toml
#         -DRAPTURE_ENGINE_DIR=path/to/rapture-engine -DSDL2_DIR=path/to/SDL2/cmake
#   cmake --build build
#   (from the game folder)  path\to\build\<game>.exe
#
# The recompiler runs at configure time on the user's own game files and writes
# the generated C and the rebased images into the build directory (never into
# the source tree). Tests: build/test_mushoot_crt build/gen, tools/difftest.py.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GAME_CONFIG "${CMAKE_SOURCE_DIR}/configs/mushoot.toml" CACHE FILEPATH "game config")
set(GAME_DIR "" CACHE PATH "folder with the user's original game files")
set(GAME_EXE "game" CACHE STRING "name of the game executable")
set(RAPTURE_ENGINE_DIR "${CMAKE_SOURCE_DIR}/../rapture-engine" CACHE PATH "rapture-engine checkout (its platform/ layer is used)")
option(RECOMP_BUILD_GAME "Build the game executable (needs rapture-engine and SDL2)" ON)
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

# Generated code and the runtime, as object libraries (they reference each other).
add_library(game_gen OBJECT ${GEN_SOURCES} "${GEN_DIR}/dispatch.c")
target_include_directories(game_gen PUBLIC "${GEN_DIR}" runtime)
# Generated code: strict aliasing is irrelevant (memcpy accessors), warnings are noise.
target_compile_options(game_gen PRIVATE -O2 -w -fno-strict-aliasing)
option(RECOMP_CALLTRACE "Record the last guest function entries per thread (printed on a fault)" OFF)
if(RECOMP_CALLTRACE)
  target_compile_definitions(game_gen PRIVATE RC_CALLTRACE=1)
endif()

add_library(recomp_runtime OBJECT runtime/runtime.c)
target_include_directories(recomp_runtime PUBLIC runtime "${GEN_DIR}")

# Tests (no Win32 layer: tests/hle_none.c provides an empty HLE table).
option(RECOMP_BUILD_TESTS "Build the test programs (test_mushoot_crt needs the MuShoot config)" ON)
if(RECOMP_BUILD_TESTS)
add_executable(test_mushoot_crt tests/test_mushoot_crt.c tests/hle_none.c
               $<TARGET_OBJECTS:game_gen> $<TARGET_OBJECTS:recomp_runtime>)
target_include_directories(test_mushoot_crt PRIVATE runtime)
enable_testing()
add_test(NAME mushoot_crt COMMAND test_mushoot_crt "${GEN_DIR}")

# Differential test runner (driven by tools/difftest.py, which needs unicorn).
add_executable(difftest tests/difftest.c tests/hle_none.c
               $<TARGET_OBJECTS:game_gen> $<TARGET_OBJECTS:recomp_runtime>)
target_include_directories(difftest PRIVATE runtime)
endif()

if(RECOMP_BUILD_GAME)
  # rapture-engine: only its library target `rapture` is needed (platform/Window for the window).
  set(RAPTURE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  if(NOT TARGET SDL2::SDL2)
    find_package(SDL2 REQUIRED CONFIG)
  endif()
  add_subdirectory("${RAPTURE_ENGINE_DIR}" rapture-engine EXCLUDE_FROM_ALL)

  file(GLOB WIN32_SOURCES CONFIGURE_DEPENDS win32/*.c win32/*.cpp)
  add_executable(${GAME_EXE} ${WIN32_SOURCES} $<TARGET_OBJECTS:game_gen> $<TARGET_OBJECTS:recomp_runtime>)
  target_include_directories(${GAME_EXE} PRIVATE runtime win32 "${RAPTURE_ENGINE_DIR}" "${GEN_DIR}")
  target_link_libraries(${GAME_EXE} PRIVATE rapture SDL2::SDL2 gdi32 winmm msimg32)
  target_compile_options(${GAME_EXE} PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wno-unused-function>)
  # The game folder of this build: the executable falls back to it when started elsewhere.
  file(TO_CMAKE_PATH "${GAME_DIR}" GAME_DIR_FWD)   # forward slashes: no escaping needed in C
  file(WRITE "${GEN_DIR}/build_info.h" "#define RC_BUILD_GAME_DIR \"${GAME_DIR_FWD}\"
")
  if(MINGW)
    target_link_options(${GAME_EXE} PRIVATE -static-libgcc -static-libstdc++)
  endif()
  # The rebased images are in build/gen, next to the executable (the default --gen).
  # SDL2.dll and the thread runtime DLL of the MinGW toolchain (if it has one) go next to it.
  get_filename_component(CC_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
  set(RUNTIME_DLLS "$<TARGET_FILE:SDL2::SDL2>")
  foreach(dll libmcfgthread-2.dll libmcfgthread-1.dll libwinpthread-1.dll)
    if(EXISTS "${CC_DIR}/${dll}")
      list(APPEND RUNTIME_DLLS "${CC_DIR}/${dll}")
    endif()
  endforeach()
  add_custom_command(TARGET ${GAME_EXE} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RUNTIME_DLLS} "$<TARGET_FILE_DIR:${GAME_EXE}>/")
endif()
