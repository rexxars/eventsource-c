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
