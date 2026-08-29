# Copyright (c) National University of Singapore.
# Licensed under the MIT License.

cmake_minimum_required(VERSION 3.16)

project(RANBooster_thor_fhaul_proxy_tests)

add_executable(test_datapath test_datapath.c)

target_include_directories(test_datapath PRIVATE
  ${RANBOOSTER_HEADERS}
  ${DPDK_INCLUDE_DIRS}
  ${PROJECT_SOURCE_DIR}/../../src/dpdk/thor_fhaul_proxy
)

target_link_libraries(test_datapath PRIVATE fhi_lib)

set_target_properties(test_datapath PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)

add_test(
  NAME datapath_unit
  COMMAND test_datapath --no-huge -m 512 --no-pci --file-prefix thor_unit
)
