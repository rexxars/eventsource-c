cmake_minimum_required(VERSION 3.16)
# In-repo builds consume the library via the wrapper component (see the
# comment in that CMakeLists for why the repo root cannot be used directly).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../ports/esp-idf/component")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Streams like Wikimedia's EventStreams use composite ids far beyond the
# default 128-byte cap. The define sizes a struct field inside the library,
# so it must apply to every component, not just main - hence a build-wide
# property rather than a target define.
idf_build_set_property(COMPILE_DEFINITIONS "SSE_CLIENT_ID_MAX=512" APPEND)
project(sse_tdisplay)
