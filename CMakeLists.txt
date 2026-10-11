cmake_minimum_required(VERSION 3.16)
project(hard_drive_monitor VERSION 0.2.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(HDMON_STATIC "Link the executable statically where the platform allows it" ON)
option(HDMON_BUILD_TESTS "Build the doctest test runner" ON)

# ---- vendored SQLite amalgamation (C)
add_library(hdmon_sqlite STATIC third_party/sqlite/sqlite3.c)
target_include_directories(hdmon_sqlite PUBLIC third_party/sqlite)
target_compile_definitions(hdmon_sqlite PRIVATE
  SQLITE_THREADSAFE=0 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_DQS=0)
if(MSVC)
  target_compile_options(hdmon_sqlite PRIVATE /w)
else()
  target_compile_options(hdmon_sqlite PRIVATE -w)
endif()

# ---- core library
add_library(hdmon_core STATIC
  src/util.cpp src/config.cpp src/smart.cpp src/health.cpp src/history.cpp
  src/alerts.cpp src/service.cpp src/render.cpp src/collect.cpp src/cli.cpp src/update.cpp)
target_include_directories(hdmon_core PUBLIC src third_party)
target_link_libraries(hdmon_core PUBLIC hdmon_sqlite)
target_compile_definitions(hdmon_core PRIVATE HDMON_VERSION="Beta V ${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(hdmon_core PRIVATE /W4 /utf-8 /EHsc)
  target_compile_definitions(hdmon_core PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(hdmon_core PRIVATE -Wall -Wextra)
endif()
if(WIN32)
  target_compile_definitions(hdmon_core PUBLIC UNICODE _UNICODE NOMINMAX)
  target_link_libraries(hdmon_core PUBLIC shell32 advapi32)
endif()
find_package(Threads REQUIRED)
target_link_libraries(hdmon_core PUBLIC Threads::Threads)

function(hdmon_static tgt)
  if(NOT MSVC AND NOT APPLE)
    target_link_options(${tgt} PRIVATE $<$<CONFIG:Release,MinSizeRel>:-s>)   # strip symbols
  endif()
  if(NOT HDMON_STATIC)
    return()
  endif()
  if(MSVC)
    set_property(TARGET ${tgt} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  elseif(WIN32)                       # MinGW: no libstdc++/libgcc/winpthread DLLs
    target_link_options(${tgt} PRIVATE -static -static-libgcc -static-libstdc++)
  elseif(APPLE)                       # macOS: no fully static binaries; libc++/libSystem ship with the OS
  else()                              # Linux: fully static
    target_link_options(${tgt} PRIVATE -static)
  endif()
endfunction()

if(MSVC)
  # Static CRT for every target so the libraries match the executables.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_property(TARGET hdmon_sqlite hdmon_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(hdmon src/main.cpp)
target_link_libraries(hdmon PRIVATE hdmon_core)
hdmon_static(hdmon)

if(WIN32)
  # GUI-subsystem twin for the scheduled task (no console window flash).
  add_executable(hdmonw WIN32 src/main.cpp)
  target_compile_definitions(hdmonw PRIVATE HDMON_GUI)
  target_link_libraries(hdmonw PRIVATE hdmon_core)
  hdmon_static(hdmonw)
endif()


install(TARGETS hdmon RUNTIME DESTINATION bin)
if(WIN32)
  install(TARGETS hdmonw RUNTIME DESTINATION bin)
endif()

# ---- tests
if(HDMON_BUILD_TESTS)
  enable_testing()
  file(GLOB HDMON_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
  add_executable(hdmon_tests ${HDMON_TEST_SOURCES})
  target_link_libraries(hdmon_tests PRIVATE hdmon_core)
  hdmon_static(hdmon_tests)
  target_compile_definitions(hdmon_tests PRIVATE HDMON_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  if(MSVC)
    target_compile_options(hdmon_tests PRIVATE /utf-8 /EHsc)
  endif()
  add_test(NAME hdmon_tests COMMAND hdmon_tests)
  add_test(NAME hdmon_version COMMAND hdmon --version)
  set_tests_properties(hdmon_version PROPERTIES PASS_REGULAR_EXPRESSION "Hard Drive Monitor \\(hdmon\\) Beta V ${PROJECT_VERSION}")
endif()
