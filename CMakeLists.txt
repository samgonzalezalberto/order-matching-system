cmake_minimum_required(VERSION 3.24)
project(order_matching_engine VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language standard / build type
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OMS_NATIVE_ARCH "Tune for the build machine (-march=native); disable for portable binaries" ON)
option(OMS_BUILD_TESTS "Build the unit / integration / benchmark test suite" ON)

# Release: -O3 (+ -march=native). Applied to our targets only, via one shared
# interface target, so third-party code keeps its own flags.
include(CheckCXXCompilerFlag)
add_library(oms_options INTERFACE)
target_compile_options(oms_options INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow
  $<$<CONFIG:Release>:-O3>)
target_compile_definitions(oms_options INTERFACE $<$<CONFIG:Release>:NDEBUG>)
if(OMS_NATIVE_ARCH)
  check_cxx_compiler_flag("-march=native" OMS_HAS_MARCH_NATIVE)
  if(OMS_HAS_MARCH_NATIVE)
    target_compile_options(oms_options INTERFACE $<$<CONFIG:Release>:-march=native>)
  else()
    message(STATUS "-march=native not supported by this compiler; skipping")
  endif()
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Dependencies (system packages are preferred; otherwise fetched, pinned)
# ---------------------------------------------------------------------------
include(FetchContent)

# cpp-httplib: header-only. TLS/compression are not needed, so disable them to
# keep the build hermetic.
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE  OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.63.0
  GIT_SHALLOW    TRUE
  FIND_PACKAGE_ARGS NAMES httplib)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.12.0
  GIT_SHALLOW    TRUE
  FIND_PACKAGE_ARGS NAMES nlohmann_json)

set(_deps httplib nlohmann_json)

if(OMS_BUILD_TESTS)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE
    FIND_PACKAGE_ARGS NAMES GTest)
  list(APPEND _deps googletest)
endif()

FetchContent_MakeAvailable(${_deps})

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
# Header-only matching core (order.hpp, order_book.hpp, matching_engine.hpp).
add_library(oms_core INTERFACE)
target_include_directories(oms_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(oms_core INTERFACE cxx_std_20)
target_link_libraries(oms_core INTERFACE Threads::Threads)

# REST layer.
add_library(oms_api STATIC src/api_server.cpp)
target_link_libraries(oms_api
  PUBLIC  oms_core
  PRIVATE oms_options httplib::httplib nlohmann_json::nlohmann_json)

# Server executable.
add_executable(order_matching_engine src/main.cpp)
target_link_libraries(order_matching_engine PRIVATE oms_api oms_options)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(OMS_BUILD_TESTS)
  enable_testing()
  include(GoogleTest)

  add_executable(test_matching_engine tests/test_matching_engine.cpp)
  target_link_libraries(test_matching_engine PRIVATE
    oms_api oms_options httplib::httplib nlohmann_json::nlohmann_json GTest::gtest_main)
  gtest_discover_tests(test_matching_engine DISCOVERY_TIMEOUT 60)
endif()
