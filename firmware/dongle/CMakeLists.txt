cmake_minimum_required(VERSION 3.16)

# Shared espkvm components (protocol, crypto, HID parsing)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../common/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espkvm_dongle)
