cmake_minimum_required(VERSION 3.16)
project(apixel VERSION 2.0.0 LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)

option(APIXEL_NATIVE "Tune for the build machine (-march=native). Never changes the archive bytes." OFF)
option(APIXEL_SANITIZE "Build with AddressSanitizer + UBSan (for tests and fuzzing)" OFF)
option(APIXEL_FUZZ "Build libFuzzer targets in fuzz/ (requires Clang)" OFF)
option(APIXEL_FAST_LINK "Link with mold when it is installed (build speed only; binaries are equivalent)" ON)
if(APIXEL_FAST_LINK)
  find_program(APIXEL_MOLD mold)
  if(APIXEL_MOLD)
    add_link_options(-fuse-ld=mold)
  endif()
endif()

find_package(Threads REQUIRED)

# Vendored suffix-array / BWT library (Apache-2.0), built as plain C.
add_library(sais STATIC third_party/libsais/libsais.c)
target_include_directories(sais PUBLIC third_party/libsais)

add_library(apixel_core STATIC
  src/analyze.cpp
  src/container.cpp
  src/stages.cpp
  src/stage_bwt.cpp
  src/stage_cm1.cpp
  src/stage_x86.cpp
  src/stage_arm64.cpp)
target_include_directories(apixel_core PUBLIC src)
target_link_libraries(apixel_core PUBLIC sais Threads::Threads)
# -fwrapv is not needed: the code has no signed overflow. No -ffast-math: there is no float in the codec.
target_compile_options(apixel_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

add_executable(apixel src/main.cpp)
target_link_libraries(apixel PRIVATE apixel_core)
target_compile_options(apixel PRIVATE -Wall -Wextra -Wpedantic)

add_executable(apixel_tests tests/unit_tests.cpp)
target_link_libraries(apixel_tests PRIVATE apixel_core)

# Developer tool for model experiments (-DAPIXEL_TUNE=ON reads model constants from the environment).
option(APIXEL_TUNE "Build tools/devtool with tunable CM1 constants and cost accounting" OFF)
if(APIXEL_TUNE)
  add_executable(devtool tools/devtool.cpp src/analyze.cpp src/stage_cm1.cpp src/stage_bwt.cpp src/stage_x86.cpp src/stage_arm64.cpp src/stages.cpp)
  target_include_directories(devtool PRIVATE src)
  target_compile_definitions(devtool PRIVATE APX_TUNE)
  target_link_libraries(devtool PRIVATE sais)
endif()

if(APIXEL_NATIVE)
  add_compile_options(-march=native)
  target_compile_options(sais PRIVATE -march=native)
  target_compile_options(apixel_core PRIVATE -march=native)
endif()

if(APIXEL_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "APIXEL_FUZZ requires Clang (clang++ / clang) with libFuzzer support")
  endif()
  # Instrument core libraries for coverage and sanitizers without linking main() from libFuzzer.
  foreach(t sais apixel_core)
    target_compile_options(${t} PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -g)
    target_link_options(${t} PRIVATE -fsanitize=address,undefined)
  endforeach()
  foreach(t apixel apixel_tests)
    target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -g)
    target_link_options(${t} PRIVATE -fsanitize=address,undefined)
  endforeach()
  if(APIXEL_TUNE)
    target_compile_options(devtool PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -g)
    target_link_options(devtool PRIVATE -fsanitize=address,undefined)
  endif()
  add_subdirectory(fuzz)
elseif(APIXEL_SANITIZE)
  foreach(t sais apixel_core apixel apixel_tests)
    target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    target_link_options(${t} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

enable_testing()
add_test(NAME unit COMMAND apixel_tests)
add_test(NAME cli COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_test.sh $<TARGET_FILE:apixel>)
