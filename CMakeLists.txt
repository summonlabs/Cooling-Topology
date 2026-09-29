# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# Cooling Topology - DCCP Tranche 3 (Cooling Infrastructure Control).
#
# The primary product surface is the library target `cooling_topology`, exported
# as `dccp::cooling_topology`. The inspection CLI, examples, tests and benchmarks
# are optional; only the library and the CLI are installed.

cmake_minimum_required(VERSION 3.20)

# Keep in sync with include/dccp/cooling_topology/version.hpp; the test suite
# compares this value against the compiled-in library version.
project(cooling_topology
        VERSION 1.0.0
        DESCRIPTION "Generation-bound structural model of data-centre cooling connectivity"
        HOMEPAGE_URL "https://github.com/summonlabs/Cooling-Topology"
        LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(COOLING_TOPOLOGY_BUILD_TESTS "Build the Cooling Topology test suite" ON)
option(COOLING_TOPOLOGY_BUILD_TOOLS "Build the ctopctl inspection tool" ON)
option(COOLING_TOPOLOGY_BUILD_EXAMPLES "Build the Cooling Topology examples" ON)
option(COOLING_TOPOLOGY_BUILD_BENCHMARKS "Build the Cooling Topology benchmarks" ON)
option(COOLING_TOPOLOGY_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(COOLING_TOPOLOGY_ENABLE_ASAN "Build with AddressSanitizer" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Warnings and sanitizers
# ---------------------------------------------------------------------------

add_library(cooling_topology_warnings INTERFACE)
add_library(dccp::cooling_topology_warnings ALIAS cooling_topology_warnings)

if(MSVC)
  target_compile_options(cooling_topology_warnings INTERFACE
    /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /MP)
  if(COOLING_TOPOLOGY_WARNINGS_AS_ERRORS)
    target_compile_options(cooling_topology_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(cooling_topology_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-align -Wunused
    -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2)
  if(COOLING_TOPOLOGY_WARNINGS_AS_ERRORS)
    target_compile_options(cooling_topology_warnings INTERFACE -Werror)
  endif()
endif()

add_library(cooling_topology_sanitizers INTERFACE)
if(COOLING_TOPOLOGY_ENABLE_ASAN)
  if(MSVC)
    # /Zi is required alongside /fsanitize=address: without debug information
    # the compiler emits C5072 on every translation unit, which a
    # warnings-as-errors build correctly refuses. Debug information is also what
    # makes an ASan report name the line it came from.
    target_compile_options(cooling_topology_sanitizers INTERFACE /fsanitize=address /Zi)
    # /fsanitize=address is a compiler-driver option: every instrumented object
    # already carries the default-library directive for the ASan runtime, and
    # passing the option to link.exe only produces LNK4044. /DEBUG is what the
    # linker asks for so an ASan report can name the line it came from.
    target_link_options(cooling_topology_sanitizers INTERFACE /DEBUG)
  else()
    target_compile_options(cooling_topology_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(cooling_topology_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer)
  endif()
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(cooling_topology
  src/version.cpp
  src/result.cpp
  src/text.cpp
  src/units.cpp
  src/strong_id.cpp
  src/digest.cpp
  src/evidence.cpp
  src/model.cpp
  src/provenance.cpp
  src/graph.cpp
  src/topology.cpp
  src/validate.cpp
  src/canonical.cpp
  src/query.cpp
  src/diff.cpp
  src/mutation.cpp
  src/import.cpp
  src/file_ops.cpp
  src/store.cpp)
add_library(dccp::cooling_topology ALIAS cooling_topology)

target_compile_features(cooling_topology PUBLIC cxx_std_20)
target_include_directories(cooling_topology
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cooling_topology
  PRIVATE
    $<BUILD_INTERFACE:cooling_topology_warnings>
    $<BUILD_INTERFACE:cooling_topology_sanitizers>)
set_target_properties(cooling_topology PROPERTIES
  CXX_EXTENSIONS OFF
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON
  EXPORT_NAME cooling_topology)

# ---------------------------------------------------------------------------
# Inspection tool
# ---------------------------------------------------------------------------

if(COOLING_TOPOLOGY_BUILD_TOOLS)
  add_executable(ctopctl tools/ctopctl/main.cpp)
  target_link_libraries(ctopctl PRIVATE dccp::cooling_topology cooling_topology_warnings cooling_topology_sanitizers)
  target_include_directories(ctopctl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  set_target_properties(ctopctl PROPERTIES CXX_EXTENSIONS OFF)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

if(COOLING_TOPOLOGY_BUILD_EXAMPLES)
  foreach(example_name IN ITEMS
      plant_redundancy
      primary_secondary_circuit
      stale_update
      durable_store)
    add_executable(cooling_topology_example_${example_name} examples/${example_name}.cpp)
    target_link_libraries(cooling_topology_example_${example_name}
      PRIVATE dccp::cooling_topology cooling_topology_warnings cooling_topology_sanitizers)
    set_target_properties(cooling_topology_example_${example_name} PROPERTIES CXX_EXTENSIONS OFF)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(COOLING_TOPOLOGY_BUILD_TESTS)
  enable_testing()

  add_library(cooling_topology_test_support STATIC
    tests/test_main.cpp
    tests/child_process.cpp)
  target_link_libraries(cooling_topology_test_support
    PUBLIC dccp::cooling_topology cooling_topology_warnings cooling_topology_sanitizers)
  target_include_directories(cooling_topology_test_support
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(cooling_topology_test_support
    PUBLIC COOLING_TOPOLOGY_CMAKE_VERSION="${PROJECT_VERSION}")
  set_target_properties(cooling_topology_test_support PROPERTIES CXX_EXTENSIONS OFF)

  add_executable(cooling_topology_tests
    tests/tests_text.cpp
    tests/tests_units.cpp
    tests/tests_digest.cpp
    tests/tests_model.cpp
    tests/tests_topology.cpp
    tests/tests_validation.cpp
    tests/tests_canonical.cpp
    tests/tests_query.cpp
    tests/tests_redundancy.cpp
    tests/tests_diff.cpp
    tests/tests_import.cpp
    tests/tests_store.cpp
    tests/tests_recovery.cpp
    tests/tests_idempotency.cpp
    tests/tests_property.cpp
    tests/tests_adversarial.cpp
    tests/tests_multiprocess.cpp
    tests/tests_cli.cpp
    tests/tests_internal.cpp)
  target_link_libraries(cooling_topology_tests PRIVATE cooling_topology_test_support)
  target_include_directories(cooling_topology_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  set_target_properties(cooling_topology_tests PROPERTIES CXX_EXTENSIONS OFF)

  if(COOLING_TOPOLOGY_BUILD_TOOLS)
    target_compile_definitions(cooling_topology_tests
      PRIVATE COOLING_TOPOLOGY_CLI_PATH="$<TARGET_FILE:ctopctl>")
    add_dependencies(cooling_topology_tests ctopctl)
  endif()

  add_test(NAME cooling_topology_tests COMMAND cooling_topology_tests)
  set_tests_properties(cooling_topology_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

if(COOLING_TOPOLOGY_BUILD_BENCHMARKS)
  add_executable(cooling_topology_benchmarks benchmarks/benchmarks.cpp)
  target_link_libraries(cooling_topology_benchmarks
    PRIVATE dccp::cooling_topology cooling_topology_warnings cooling_topology_sanitizers)
  set_target_properties(cooling_topology_benchmarks PROPERTIES CXX_EXTENSIONS OFF)
endif()

# ---------------------------------------------------------------------------
# Install and export
# ---------------------------------------------------------------------------

install(TARGETS cooling_topology
  EXPORT cooling_topologyTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(COOLING_TOPOLOGY_BUILD_TOOLS)
  install(TARGETS ctopctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(COOLING_TOPOLOGY_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/cooling_topology)

install(EXPORT cooling_topologyTargets
  FILE cooling_topologyTargets.cmake
  NAMESPACE dccp::
  DESTINATION ${COOLING_TOPOLOGY_CMAKE_INSTALL_DIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cooling_topologyConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/cooling_topologyConfig.cmake
  INSTALL_DESTINATION ${COOLING_TOPOLOGY_CMAKE_INSTALL_DIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/cooling_topologyConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/cooling_topologyConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/cooling_topologyConfigVersion.cmake
  DESTINATION ${COOLING_TOPOLOGY_CMAKE_INSTALL_DIR})

install(FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/README.md
  ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
  ${CMAKE_CURRENT_SOURCE_DIR}/NOTICE
  DESTINATION ${CMAKE_INSTALL_DOCDIR})
