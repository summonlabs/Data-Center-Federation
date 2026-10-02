# Data Center Federation - federation membership, delegation and partition reconciliation runtime.
# Copyright 2026 Summon Software Labs. Apache License 2.0.
cmake_minimum_required(VERSION 3.21)

project(data-center-federation
  VERSION 1.0.0
  DESCRIPTION "Federation membership, delegation and partition reconciliation runtime for data center control planes"
  HOMEPAGE_URL "https://github.com/summonlabs/Data-Center-Federation"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(DCF_BUILD_TOOLS "Build the federation, site, relay and control command line programs" ON)
option(DCF_BUILD_TESTS "Build the Data Center Federation test suites" ${PROJECT_IS_TOP_LEVEL})
option(DCF_BUILD_BENCHMARKS "Build the Data Center Federation benchmark harness" ${PROJECT_IS_TOP_LEVEL})
option(DCF_WARNINGS_AS_ERRORS "Treat first-party compiler diagnostics as errors" ON)
option(DCF_ENABLE_ASAN "Build and link with AddressSanitizer where the toolchain supports it" OFF)
option(DCF_ENABLE_UBSAN "Build and link with UndefinedBehaviorSanitizer where the toolchain supports it" OFF)

if(DCF_ENABLE_UBSAN AND MSVC)
  message(STATUS "DCF_ENABLE_UBSAN has no MSVC equivalent and is ignored on this toolchain")
  set(DCF_ENABLE_UBSAN OFF)
endif()

# ---------------------------------------------------------------------------
# Language and binary defaults
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  # /RTC1 is incompatible with /fsanitize=address.
  if(DCF_ENABLE_ASAN)
    foreach(dcf_flag_var CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELWITHDEBINFO)
      string(REPLACE "/RTC1" "" ${dcf_flag_var} "${${dcf_flag_var}}")
    endforeach()
  endif()
endif()

# ---------------------------------------------------------------------------
# Warning policy. First-party code must compile clean at this level.
# ---------------------------------------------------------------------------
function(dcf_apply_warning_policy target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4
      /permissive-
      /utf-8
      /EHsc
      /Zc:__cplusplus
      /Zc:preprocessor
      /Zc:inline
      /diagnostics:caret)
    if(DCF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(DCF_ENABLE_ASAN)
      target_compile_options(${target} PRIVATE /fsanitize=address)
      target_link_options(${target} PRIVATE /fsanitize=address)
    endif()
    set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wconversion
      -Wsign-conversion
      -Wold-style-cast
      -Wnon-virtual-dtor
      -Woverloaded-virtual
      -Wnull-dereference
      -Wdouble-promotion
      -Wformat=2)
    if(DCF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    # flock(2), fsync(2) and posix_spawn(3) are declared only while
    # _DEFAULT_SOURCE (or _GNU_SOURCE) is in effect. CMAKE_CXX_EXTENSIONS is
    # OFF, so the compiler is invoked with -std=c++20 and __STRICT_ANSI__ is
    # defined, which would otherwise hide them.
    target_compile_definitions(${target} PRIVATE _DEFAULT_SOURCE)
    set(dcf_sanitizers "")
    if(DCF_ENABLE_ASAN)
      list(APPEND dcf_sanitizers address)
    endif()
    if(DCF_ENABLE_UBSAN)
      list(APPEND dcf_sanitizers undefined)
      # Undefined behaviour is a defect, not a recoverable event: stop at the
      # first finding rather than logging it and carrying on.
      target_compile_options(${target} PRIVATE -fno-sanitize-recover=all)
    endif()
    if(dcf_sanitizers)
      string(JOIN "," dcf_sanitizer_list ${dcf_sanitizers})
      target_compile_options(${target} PRIVATE -fsanitize=${dcf_sanitizer_list} -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=${dcf_sanitizer_list})
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
# The runtime owns threads and synchronises with mutexes and condition
# variables. On POSIX those symbols live in the system thread library, and a
# downstream consumer linking the installed archive needs exactly the same
# dependency, so it is declared PUBLIC and is re-exported through the package.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(DCF_CORE_SOURCES
  src/version.cpp
  src/types.cpp
  src/hash.cpp
  src/codec.cpp
  src/capability.cpp
  src/delegation.cpp
  src/membership.cpp
  src/reconciliation.cpp
  src/command.cpp
  src/record_codec.cpp
  src/state.cpp
  src/engine.cpp
  src/platform.cpp
  src/store.cpp
  src/runtime.cpp
  src/wire.cpp
  src/json.cpp
  src/report.cpp
  src/cli.cpp
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # GCC 14 at -O3 reports -Wfree-nonheap-object inside libstdc++'s
  # new_allocator.h for these two translation units, which use std::variant and
  # std::vector in the ordinary way. The diagnostic names a pointer it cannot
  # identify, it disappears at -O2 and below, and it points into a standard
  # library header rather than into this code, so it is a compiler artefact and
  # not a defect here. The suppression is attached to exactly these two files;
  # no other file and no other compiler is affected, and nothing else is
  # suppressed anywhere.
  set_source_files_properties(src/store.cpp src/wire.cpp PROPERTIES
    COMPILE_OPTIONS "-Wno-free-nonheap-object")
endif()

add_library(dcf STATIC ${DCF_CORE_SOURCES})
add_library(DataCenterFederation::dcf ALIAS dcf)

target_link_libraries(dcf PUBLIC Threads::Threads)

if(WIN32)
  # The socket layer is a system library on Windows and a consumer of the
  # installed archive needs the same one, so it is part of the exported interface.
  target_link_libraries(dcf PUBLIC ws2_32)
endif()

target_include_directories(dcf
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

dcf_apply_warning_policy(dcf)

# ---------------------------------------------------------------------------
# Programs
# ---------------------------------------------------------------------------
if(DCF_BUILD_TOOLS)
  set(DCF_PROGRAMS
    dcf-federationd:apps/federationd.cpp
    dcf-sited:apps/sited.cpp
    dcf-relay:tools/dcf_relay.cpp
    dcfctl:apps/dcfctl.cpp)

  foreach(dcf_program IN LISTS DCF_PROGRAMS)
    string(REPLACE ":" ";" dcf_program_parts "${dcf_program}")
    list(GET dcf_program_parts 0 dcf_program_name)
    list(GET dcf_program_parts 1 dcf_program_source)
    add_executable(${dcf_program_name} ${dcf_program_source})
    target_link_libraries(${dcf_program_name} PRIVATE dcf)
    dcf_apply_warning_policy(${dcf_program_name})
    install(TARGETS ${dcf_program_name} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(DCF_BUILD_TESTS)
  enable_testing()

  add_library(dcf_test_support STATIC
    tests/support/test_harness.cpp
    tests/support/process.cpp)
  target_include_directories(dcf_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
  target_link_libraries(dcf_test_support PUBLIC dcf)
  dcf_apply_warning_policy(dcf_test_support)

  # Test sources are discovered rather than listed so that adding a suite is a
  # single-file change. CONFIGURE_DEPENDS makes the build regenerate when a file
  # appears or disappears.
  file(GLOB dcf_test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")

  foreach(dcf_test_source IN LISTS dcf_test_sources)
    get_filename_component(dcf_test_name "${dcf_test_source}" NAME_WE)
    add_executable(${dcf_test_name} "${dcf_test_source}")
    target_link_libraries(${dcf_test_name} PRIVATE dcf dcf_test_support)
    target_compile_definitions(${dcf_test_name} PRIVATE
      DCF_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
      DCF_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    if(DCF_BUILD_TOOLS)
      # The multiprocess suite launches these as real processes rather than
      # linking them, which is the whole point of the suite.
      target_compile_definitions(${dcf_test_name} PRIVATE
        DCF_FEDERATIOND_PATH="$<TARGET_FILE:dcf-federationd>"
        DCF_SITED_PATH="$<TARGET_FILE:dcf-sited>"
        DCF_RELAY_PATH="$<TARGET_FILE:dcf-relay>"
        DCF_CTL_PATH="$<TARGET_FILE:dcfctl>")
      add_dependencies(${dcf_test_name} dcf-federationd dcf-sited dcf-relay dcfctl)
    endif()
    dcf_apply_warning_policy(${dcf_test_name})
    add_test(NAME ${dcf_test_name} COMMAND ${dcf_test_name})
    # Each suite runs in a scratch directory of its own so that suites which
    # create files cannot collide when CTest runs them in parallel. No test is
    # given a timeout: a hang is a defect to diagnose, not something to kill.
    add_custom_command(TARGET ${dcf_test_name} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory
              "${CMAKE_CURRENT_BINARY_DIR}/scratch/${dcf_test_name}")
    set_tests_properties(${dcf_test_name} PROPERTIES
      LABELS "${dcf_test_name}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/scratch/${dcf_test_name}")
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(DCF_BUILD_BENCHMARKS)
  add_executable(bench_federation bench/bench_federation.cpp)
  target_link_libraries(bench_federation PRIVATE dcf)
  dcf_apply_warning_policy(bench_federation)
  add_custom_command(TARGET bench_federation POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/bench-scratch")
  add_custom_target(bench
    COMMAND bench_federation --scratch "${CMAKE_CURRENT_BINARY_DIR}/bench-scratch"
    DEPENDS bench_federation
    COMMENT "Running the Data Center Federation benchmark harness")
endif()

# ---------------------------------------------------------------------------
# Install and export
# ---------------------------------------------------------------------------
install(TARGETS dcf
  EXPORT DataCenterFederationTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# The package directory must be named after the package. CMake searches
# <prefix>/lib/cmake/<name>*/ for <name>Config.cmake, and a directory named
# "data-center-federation" does not match the package name
# "DataCenterFederation".
set(DCF_CMAKE_PACKAGE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/DataCenterFederation")

install(EXPORT DataCenterFederationTargets
  FILE DataCenterFederationTargets.cmake
  NAMESPACE DataCenterFederation::
  DESTINATION "${DCF_CMAKE_PACKAGE_DIR}")

configure_package_config_file(
  cmake/DataCenterFederationConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/DataCenterFederationConfig.cmake"
  INSTALL_DESTINATION "${DCF_CMAKE_PACKAGE_DIR}")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/DataCenterFederationConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/DataCenterFederationConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/DataCenterFederationConfigVersion.cmake"
  DESTINATION "${DCF_CMAKE_PACKAGE_DIR}")
