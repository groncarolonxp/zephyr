# Copyright 2026 NXP
# SPDX-License-Identifier: Apache-2.0
#
# Pull a stock Zephyr application (default: samples/net/sockets/echo) into the
# build as the "remote" (cpu1) image, so this bridge trampoline on cpu0 and the
# unmodified socket app on cpu1 are built and flashed together by sysbuild.

if("${SB_CONFIG_REMOTE_BOARD}" STREQUAL "")
  message(FATAL_ERROR "Target ${BOARD}/${BOARD_QUALIFIERS} not supported for "
    "this sample. There is no remote board selected in Kconfig.sysbuild")
endif()

if("${SB_CONFIG_REMOTE_APP_SOURCE_DIR}" STREQUAL "")
  message(FATAL_ERROR "No remote application source directory selected in "
    "Kconfig.sysbuild (SB_CONFIG_REMOTE_APP_SOURCE_DIR)")
endif()

set(REMOTE_APP remote)

ExternalZephyrProject_Add(
  APPLICATION ${REMOTE_APP}
  SOURCE_DIR  ${SB_CONFIG_REMOTE_APP_SOURCE_DIR}
  BOARD       ${SB_CONFIG_REMOTE_BOARD}
  BOARD_REVISION ${BOARD_REVISION}
)

# cpu0 is the host: it publishes the shared block and bridges vnet0 to the real
# Ethernet. Build the remote (cpu1) image first so the main core can reference
# its image info if needed.
add_dependencies(${DEFAULT_IMAGE} ${REMOTE_APP})
