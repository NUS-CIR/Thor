# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

cmake_minimum_required(VERSION 3.16)

project(RANBooster_thor_fhaul_proxy_dpdk)

find_package(Threads REQUIRED)

# Add the executable
add_executable(thor_fhaul_proxy_dpdk
  thor_fhaul_proxy.c
  thor_ctrl.c
)

# Add include directories
target_include_directories(thor_fhaul_proxy_dpdk PRIVATE
  ${RANBOOSTER_HEADERS}
  ${DPDK_INCLUDE_DIRS}
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(thor_fhaul_proxy_dpdk PRIVATE fhi_lib Threads::Threads)

# Optionally, set output directory
set_target_properties(thor_fhaul_proxy_dpdk PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/thor_fhaul_proxy_dpdk
)

# Expose the proxy's own directory so the unit tests can include its headers.
set(THOR_FHAUL_PROXY_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "")
