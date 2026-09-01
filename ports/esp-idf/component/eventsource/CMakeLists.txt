# Wrapper component for the IN-REPO apps (examples/esp32, tests/qemu).
#
# ESP-IDF derives a component's name from its directory basename
# (__component_add in tools/cmake/component.cmake), so the repository root
# cannot be consumed as a path dependency: its basename is whatever the
# checkout happens to be called. This directory is reliably named
# `eventsource` and registers the same sources as the root CMakeLists' ESP
# branch — keep the two lists in sync.
#
# Registry consumers never see this: they get the packed repository root
# under managed_components/rexxars__eventsource/ where the basename matches.
idf_component_register(
  SRCS
    "../../../../src/sse_parser.c"
    "../../../../src/sse_client.c"
    "../../sse_transport_esp.c"
    "../../sse_client_task.c"
  INCLUDE_DIRS "../../../../include" "../.."
  PRIV_REQUIRES esp_http_client mbedtls
)
