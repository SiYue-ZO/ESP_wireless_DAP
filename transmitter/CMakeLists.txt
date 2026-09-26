cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common")
set(PROJECT_VER "0.1.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wireless_dap_transmitter)
