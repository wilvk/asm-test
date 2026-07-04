# Packaging scaffolding (see docs/packaging.md). The C++ binding is header-only
# (asmtest.hpp over the C++-consumable C headers); the framework native library
# is *linked*, not bundled, so consumers install it once (`make install-shared`)
# and find it via pkg-config. This file makes the header an installable,
# find_package()-able INTERFACE target. `make cpp-package` tarballs the header +
# this file for distribution.
cmake_minimum_required(VERSION 3.14)
project(asmtest-cpp VERSION 1.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

add_library(asmtest_cpp INTERFACE)
add_library(asmtest::cpp ALIAS asmtest_cpp)
target_compile_features(asmtest_cpp INTERFACE cxx_std_17)
target_include_directories(asmtest_cpp INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# Locate the installed framework via pkg-config (asmtest.pc / asmtest-emu.pc from
# `make install` / `install-shared-emu`). Optional so configure succeeds before
# the framework is installed; link it when found.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(ASMTEST IMPORTED_TARGET asmtest-emu asmtest)
  if(ASMTEST_FOUND)
    target_link_libraries(asmtest_cpp INTERFACE PkgConfig::ASMTEST)
  else()
    message(STATUS "asmtest pkg-config not found; install with `make install-shared-emu`")
  endif()
endif()

install(FILES asmtest.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
