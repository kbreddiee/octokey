cmake_minimum_required(VERSION 3.16)

# Shared espkvm components (protocol, crypto, ESP-NOW link/pairing)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../common/components")

# Build only main + what it actually REQUIREs (default is every component
# found in EXTRA_COMPONENT_DIRS, which drags web/USB code into apps that
# never use it).
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espkvm_hub_air)
