cmake_minimum_required(VERSION 3.16)
# In-repo builds consume the library via the wrapper component (see the
# comment in that CMakeLists for why the repo root cannot be used directly).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../ports/esp-idf/component")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sse_example)
