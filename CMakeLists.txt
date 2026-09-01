cmake_minimum_required(VERSION 3.16)

if(ESP_PLATFORM)
  # ESP-IDF component build. Sources grow in Task 12.
  idf_component_register(
    SRCS src/sse_parser.c
    INCLUDE_DIRS include
  )
  return()
endif()

project(eventsource C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(SSE_BUILD_TESTS "Build host tests" ON)
option(SSE_SANITIZE "Build with ASan/UBSan" ON)

if(APPLE)
  # ASan-instrumented binaries hang for minutes at startup with Apple
  # clang 17 on macOS 26; UBSan alone stays fast. CI runs the full pair.
  set(SSE_SANITIZERS "undefined" CACHE STRING "Sanitizers when SSE_SANITIZE is ON")
else()
  set(SSE_SANITIZERS "address,undefined" CACHE STRING "Sanitizers when SSE_SANITIZE is ON")
endif()
if(SSE_SANITIZE)
  add_compile_options(-fsanitize=${SSE_SANITIZERS} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${SSE_SANITIZERS})
endif()

add_library(eventsource src/sse_parser.c)
target_include_directories(eventsource PUBLIC include)
target_compile_options(eventsource PRIVATE -Wall -Wextra -Werror)

if(SSE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

option(SSE_BUILD_FUZZERS "Build libFuzzer harnesses (needs clang)" OFF)
if(SSE_BUILD_FUZZERS)
  add_executable(fuzz_parser fuzz/fuzz_parser.c src/sse_parser.c)
  target_include_directories(fuzz_parser PRIVATE include)
  target_compile_options(fuzz_parser PRIVATE -fsanitize=fuzzer,address,undefined -g -O1)
  target_link_options(fuzz_parser PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

# Smoke driver for toolchains without libFuzzer (e.g. Apple clang):
# replays crash files and runs a deterministic random corpus.
option(SSE_BUILD_FUZZ_SMOKE "Build the standalone fuzz smoke driver" OFF)
if(SSE_BUILD_FUZZ_SMOKE)
  add_executable(fuzz_smoke fuzz/fuzz_parser.c fuzz/standalone_main.c)
  target_link_libraries(fuzz_smoke PRIVATE eventsource)
endif()
