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

# --- Throwaway extension-load probe (taint tier, Increment 2) ---------------
# Opt-in only (-DASMTEST_BUILD_DREXT_PROBE=ON): the sole client here that DOES
# use_DynamoRIO_extension(), to empirically test whether the prebuilt release
# extensions load under DR's private loader — the documented-but-never-tested
# blocker gating the whole Phase-5 taint re-platform. It is NOT a product artifact
# and is never built by the normal `make drtrace-client` path (which stays raw
# core API). See drclient/probe_extensions.c and
# docs/internal/analysis/dr-extension-load-probe-findings.md.
#
# Default stack is the BSD-clean set only: drmgr + drreg + drx (the drx_buf
# trace-buffer API lives in drx.h — there is no separate drx_buf extension).
# umbra is LGPL-2.1 (it ships under the Dr. Memory Framework, not DR core), so it
# is gated behind a SECOND opt-in (-DPROBE_UMBRA=ON) and stays out of the default
# probe and the CI gate.
option(ASMTEST_BUILD_DREXT_PROBE "Build the throwaway DR extension-load probe" OFF)
option(PROBE_UMBRA "Additionally link umbra (LGPL-2.1) into the probe" OFF)
if(ASMTEST_BUILD_DREXT_PROBE)
  add_library(asmtest_drext_probe SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/probe_extensions.c)
  configure_DynamoRIO_client(asmtest_drext_probe)
  use_DynamoRIO_extension(asmtest_drext_probe drmgr)
  use_DynamoRIO_extension(asmtest_drext_probe drreg)
  use_DynamoRIO_extension(asmtest_drext_probe drx)
  if(PROBE_UMBRA)
    # umbra lives in the Dr. Memory Framework, a separate find_package that
    # registers the umbra ext include dir so use_DynamoRIO_extension resolves it.
    find_package(DrMemoryFramework REQUIRED)
    use_DynamoRIO_extension(asmtest_drext_probe umbra)
    target_compile_definitions(asmtest_drext_probe PRIVATE PROBE_UMBRA)
  endif()
  set_target_properties(asmtest_drext_probe PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${ASMTEST_BUILD_DIR}")
endif()
