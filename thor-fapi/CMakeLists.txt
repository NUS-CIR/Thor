cmake_minimum_required(VERSION 3.16)
project(THOR VERSION 1.0 LANGUAGES C CXX)
include(CTest)
set(CMAKE_CXX_STANDARD 17)
option(ENABLE_MICROBENCHMARK "Record proxy P7 processing latency events" OFF)

set(BASE_DIR ${CMAKE_SOURCE_DIR})

####################################
# Git version and date extraction
####################################
set(THOR_GIT_COMMIT_HASH "" CACHE STRING "Git revision embedded in the binaries")
set(THOR_GIT_COMMIT_DATE "" CACHE STRING "Git revision date embedded in the binaries")
find_package(Git QUIET)

if(NOT THOR_GIT_COMMIT_HASH AND Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
  execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      OUTPUT_VARIABLE THOR_GIT_COMMIT_HASH
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
  )
endif()

if(NOT THOR_GIT_COMMIT_DATE AND Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
  execute_process(
      COMMAND ${GIT_EXECUTABLE} log -1 --format=%cd --date=format:%Y-%m-%d\ %H:%M:%S
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      OUTPUT_VARIABLE THOR_GIT_COMMIT_DATE
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
  )
endif()

if(NOT THOR_GIT_COMMIT_HASH)
  set(THOR_GIT_COMMIT_HASH "unknown")
endif()
if(NOT THOR_GIT_COMMIT_DATE)
  set(THOR_GIT_COMMIT_DATE "unknown")
endif()

# display or use the variables
message(STATUS "Git Version: ${THOR_GIT_COMMIT_HASH} (${THOR_GIT_COMMIT_DATE})")

# pass these to your C++ code
add_definitions("-DGIT_COMMIT_HASH=\"${THOR_GIT_COMMIT_HASH}\"")
add_definitions("-DGIT_COMMIT_DATE=\"${THOR_GIT_COMMIT_DATE}\"")

# get build time
string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M:%S")
add_definitions("-DBUILD_TIME=\"${BUILD_TIME}\"")

include_directories("${BASE_DIR}/include")

####################################
# OAI nFAPI
####################################
include_directories("${BASE_DIR}/oai-include")

set (NFAPI_DIR       ${CMAKE_SOURCE_DIR}/oai-nfapi/open-nFAPI)
set (NFAPI_USER_DIR  ${CMAKE_SOURCE_DIR}/oai-nfapi/oai_integration)

find_package(ZLIB REQUIRED)

include_directories("${NFAPI_DIR}/nfapi/public_inc")
include_directories("${NFAPI_DIR}/common/public_inc")
include_directories("${NFAPI_DIR}/pnf/public_inc")
include_directories("${NFAPI_DIR}/nfapi/inc")
include_directories("${NFAPI_DIR}/sim_common/inc")
include_directories("${NFAPI_DIR}/pnf_sim/inc")
include_directories("${NFAPI_USER_DIR}")

set(NFAPI_SRC
  ${NFAPI_DIR}/nfapi/src/nfapi_p4.c
  ${NFAPI_DIR}/nfapi/src/nfapi_p5.c
  ${NFAPI_DIR}/nfapi/src/nfapi_p7.c
)
add_library(NFAPI_LIB ${NFAPI_SRC})
target_link_libraries(NFAPI_LIB PUBLIC nfapi_common)
target_link_libraries(NFAPI_LIB PUBLIC nr_fapi_p5 nr_fapi_p7)
target_include_directories(NFAPI_LIB PUBLIC ${ZLIB_INCLUDE_DIRS})
target_link_libraries(NFAPI_LIB PUBLIC ${ZLIB_LIBRARIES})

add_compile_definitions(MAX_NUM_CCs=1)
add_compile_definitions(ENABLE_SOCKET)
include_directories(${NFAPI_DIR}/nfapi/public_inc)
include_directories(${NFAPI_DIR}/nfapi/inc)

add_subdirectory(oai-nfapi)

####################################
# nfapi-proxy
####################################
add_executable(nfapi-proxy  
                nfapi-proxy.c 
                include/nfapi-proxy-common.c
                include/log.c
                include/microbenchmark.c
                include/nfapi_p7_downlink_processor.c
                include/nfapi_p7_uplink_processor.c
              )

target_link_libraries(nfapi-proxy pthread sctp NFAPI_LIB)

target_compile_definitions(nfapi-proxy PRIVATE MAX_NUM_PNF=2)
if(ENABLE_MICROBENCHMARK)
  target_compile_definitions(nfapi-proxy PRIVATE MICROBENCHMARK_LOGGING)
endif()

# target_compile_options(nfapi-proxy PRIVATE -mcmodel=medium)

# add address sanitizer
# target_compile_options(nfapi-proxy PUBLIC -fsanitize=address -fno-omit-frame-pointer -g)
# target_link_options(nfapi-proxy PUBLIC -fsanitize=address)

# add debug
# target_compile_options(nfapi-proxy PRIVATE -g -O0)

# enable staged HARQ uplink mapping
# target_compile_definitions(nfapi-proxy PRIVATE STAGED_HARQ_UPLINK)

# use legacy downlink implementation
# target_compile_definitions(nfapi-proxy PRIVATE DL_LEGACY)

# use legacy UL_TTI.request implementation
# target_compile_definitions(nfapi-proxy PRIVATE UL_TTI_LEGACY)

# use segmentation and reassembly for UL traffic
target_compile_definitions(nfapi-proxy PRIVATE UL_SEGMENTATION_REASSEMBLY)

####################################
# Test stubs
####################################
add_executable(test_vnf_stub tests/test_vnf_stub.c tests/test_stub_utils.c)
target_link_libraries(test_vnf_stub NFAPI_LIB)
target_compile_options(test_vnf_stub PRIVATE -g -O0)

add_executable(test_pnf_stub tests/test_pnf_stub.c tests/test_stub_utils.c)
target_link_libraries(test_pnf_stub NFAPI_LIB)
target_compile_options(test_pnf_stub PRIVATE -g -O0)

####################################
# Control client
####################################
add_executable(control_client util/control_client.c)
target_compile_options(control_client PRIVATE -g -O0)


####################################
# Unit tests
####################################
if(BUILD_TESTING)
  add_executable(nfapi_proxy_unit_tests
                 tests/test_nfapi_proxy.c
                 include/nfapi-proxy-common.c
                 include/log.c
                 include/microbenchmark.c
                 include/nfapi_p7_downlink_processor.c
                 include/nfapi_p7_uplink_processor.c
                )
  target_link_libraries(nfapi_proxy_unit_tests pthread sctp NFAPI_LIB)
  # Exercise the production optimized paths: current DL/TX mirroring (no legacy
  # definitions) and raw uplink segmentation/reassembly.
  target_compile_definitions(nfapi_proxy_unit_tests PRIVATE
                             MAX_NUM_PNF=2
                             UL_SEGMENTATION_REASSEMBLY)
  # Keep assert-based checks active even when the surrounding container or CI
  # build uses CMAKE_BUILD_TYPE=Release.
  target_compile_options(nfapi_proxy_unit_tests PRIVATE -Wall -Wextra -UNDEBUG)
  add_test(NAME nfapi_proxy_unit_tests COMMAND nfapi_proxy_unit_tests)
endif()
