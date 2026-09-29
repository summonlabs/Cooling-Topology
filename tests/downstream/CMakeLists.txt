# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# Out-of-tree consumer of an INSTALLED cooling_topology package.
#
# This project is deliberately not part of the library's own build. It proves
# that a third party can consume the exported package exactly as documented:
#
#   find_package(cooling_topology 1.0 REQUIRED)
#   target_link_libraries(app PRIVATE dccp::cooling_topology)
#
# Configure it with CMAKE_PREFIX_PATH pointing at the install prefix:
#
#   cmake -G "Visual Studio 17 2022" -A x64 -S tests/downstream -B <build> -DCMAKE_PREFIX_PATH=<prefix>

cmake_minimum_required(VERSION 3.20)

# A single-configuration generator (Ninja, Makefiles) has no default build type,
# and the installed package is a Release build. Choose Release before the
# project() call so that the consumer's runtime library and iterator debug level
# match the library it links against unless the caller says otherwise.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

project(cooling_topology_downstream
        VERSION 1.0.0
        DESCRIPTION "Out-of-tree consumer of the installed cooling_topology package"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

message(STATUS "cooling_topology_downstream: project version ${PROJECT_VERSION} (build type ${CMAKE_BUILD_TYPE})")
message(STATUS "cooling_topology_downstream: requiring cooling_topology 1.0 (search path: ${CMAKE_PREFIX_PATH})")

find_package(cooling_topology 1.0 REQUIRED)

if(NOT cooling_topology_VERSION)
  message(FATAL_ERROR "cooling_topology_downstream: the found package reported no version")
endif()

message(STATUS "cooling_topology_downstream: found cooling_topology ${cooling_topology_VERSION}")
message(STATUS "cooling_topology_downstream: package directory ${cooling_topology_DIR}")
message(STATUS "cooling_topology_downstream: include directories ${COOLING_TOPOLOGY_INCLUDE_DIRS}")
message(STATUS "cooling_topology_downstream: libraries ${COOLING_TOPOLOGY_LIBRARIES}")
message(STATUS "cooling_topology_downstream: compiler ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

add_executable(downstream_consumer main.cpp)

target_link_libraries(downstream_consumer PRIVATE dccp::cooling_topology)

target_compile_features(downstream_consumer PRIVATE cxx_std_20)

# The version the package was found at, baked in so the program can prove at run
# time that the headers and the library it linked against are one release.
target_compile_definitions(downstream_consumer PRIVATE
  COOLING_TOPOLOGY_PACKAGE_VERSION="${cooling_topology_VERSION}")

set_target_properties(downstream_consumer PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
  target_compile_options(downstream_consumer PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /WX)
else()
  target_compile_options(downstream_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

message(STATUS "cooling_topology_downstream: target downstream_consumer configured")
