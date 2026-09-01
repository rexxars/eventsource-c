cmake_minimum_required(VERSION 3.16)

if(ESP_PLATFORM)
  idf_component_register(
    SRCS
      src/sse_parser.c
      src/sse_client.c
      ports/esp-idf/sse_transport_esp.c
      ports/esp-idf/sse_client_task.c
    INCLUDE_DIRS include ports/esp-idf
    PRIV_REQUIRES esp_http_client mbedtls
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

add_library(eventsource src/sse_parser.c src/sse_client.c)
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

option(SSE_BUILD_POSIX_PORT "Build the libcurl POSIX transport + example" ON)
if(SSE_BUILD_POSIX_PORT)
  # 7.62 introduced the CURLU URL API, used for same-origin redirect checks.
  find_package(CURL 7.62 REQUIRED)
  add_library(eventsource_posix ports/posix/sse_transport_curl.c ports/posix/sse_clock_posix.c)
  target_include_directories(eventsource_posix PUBLIC ports/posix)
  target_link_libraries(eventsource_posix PUBLIC eventsource CURL::libcurl)
  target_compile_options(eventsource_posix PRIVATE -Wall -Wextra -Werror)

  add_executable(example_posix examples/posix/main.c)
  target_link_libraries(example_posix PRIVATE eventsource_posix)

  # Live integration suite: fixture server + example client, covering the
  # transport behaviors unit tests cannot reach (redirect policy, 1xx
  # blocks, header-only opens, big redirect bodies, 204).
  find_program(SSE_PYTHON3 python3)
  if(SSE_BUILD_TESTS AND SSE_PYTHON3)
    add_test(NAME posix_integration
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_posix.sh
                     $<TARGET_FILE:example_posix>
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/sse_fixture_server.py
                     ${SSE_PYTHON3})
    set_tests_properties(posix_integration PROPERTIES TIMEOUT 120)
  endif()
endif()
