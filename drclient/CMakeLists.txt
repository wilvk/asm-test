# CMake sub-build for the DynamoRIO native-trace client (libasmtest_drclient.so).
#
# DynamoRIO ships no pkg-config file and requires find_package(DynamoRIO), so the
# client is the lone CMake-driven artifact the top-level Makefile shells out to
# (every other optional dep uses pkg-config). Point it at the DR cmake config dir:
#
#   cmake -DDynamoRIO_DIR=/path/to/DynamoRIO/cmake <srcdir>
#   cmake --build .
#
# C and CXX are both enabled because DynamoRIO's bundled DrMemoryFramework config
# reads CMAKE_CXX_SIZEOF_DATA_PTR; a C-only project leaves it empty and aborts.
cmake_minimum_required(VERSION 3.10)
project(asmtest_drclient C CXX)

find_package(DynamoRIO REQUIRED)

add_library(asmtest_drclient SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../src/drtrace_client.c)
configure_DynamoRIO_client(asmtest_drclient)
# Raw core API only — no use_DynamoRIO_extension(): the prebuilt drmgr/drwrap/drreg
# fail to load under DR's private loader on modern glibc, and avoiding drwrap keeps
# the tier free of its LGPL-2.1 obligation.

# The data-flow L0 VALUE client (Phase 5): a SEPARATE in-process client that records
# per-instruction register/memory value snapshots (dr_get_mcontext + decode +
# opnd_compute_address + dr_safe_read) rather than block coverage. Same raw core-API
# constraint as the control client. It includes the shared capture ABI header from
# ../src (dataflow_dr.h — <stdint.h>-only, safe alongside dr_api.h), so add that dir.
add_library(asmtest_drval_client SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../src/dataflow_dr_client.c)
configure_DynamoRIO_client(asmtest_drval_client)
target_include_directories(asmtest_drval_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# Emit straight into the asm-test build dir so the Makefile/tests find them.
set_target_properties(asmtest_drclient asmtest_drval_client PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${ASMTEST_BUILD_DIR}")
