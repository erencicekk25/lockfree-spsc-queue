cmake_minimum_required(VERSION 3.16)

project(lockfree_spsc_queue
    VERSION 1.0.0
    DESCRIPTION "Header-only lock-free single-producer/single-consumer ring buffer"
    LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(SPSC_IS_TOP_LEVEL ON)
else()
    set(SPSC_IS_TOP_LEVEL OFF)
endif()

option(SPSC_BUILD_TESTS "Build the lockfree-spsc-queue test suite" ${SPSC_IS_TOP_LEVEL})
option(SPSC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${SPSC_IS_TOP_LEVEL})

# ---------------------------------------------------------------------------
# Header-only library target
# ---------------------------------------------------------------------------
add_library(lockfree_spsc_queue INTERFACE)
add_library(lockfree_spsc::spsc_queue ALIAS lockfree_spsc_queue)

target_include_directories(lockfree_spsc_queue INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(lockfree_spsc_queue INTERFACE cxx_std_17)

# ---------------------------------------------------------------------------
# Strict warnings for in-tree targets (never propagated to consumers)
# ---------------------------------------------------------------------------
add_library(spsc_project_warnings INTERFACE)
if(MSVC)
    target_compile_options(spsc_project_warnings INTERFACE /W4 /permissive-)
    if(SPSC_WARNINGS_AS_ERRORS)
        target_compile_options(spsc_project_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(spsc_project_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual)
    if(SPSC_WARNINGS_AS_ERRORS)
        target_compile_options(spsc_project_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SPSC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS lockfree_spsc_queue EXPORT lockfree_spsc_queue_targets)
install(EXPORT lockfree_spsc_queue_targets
    NAMESPACE lockfree_spsc::
    FILE lockfree_spsc_queueTargets.cmake
    DESTINATION ${CMAKE_INSTALL_DATADIR}/lockfree_spsc_queue/cmake)

configure_package_config_file(
    cmake/lockfree_spsc_queueConfig.cmake.in
    ${PROJECT_BINARY_DIR}/lockfree_spsc_queueConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_DATADIR}/lockfree_spsc_queue/cmake)
write_basic_package_version_file(
    ${PROJECT_BINARY_DIR}/lockfree_spsc_queueConfigVersion.cmake
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)
install(FILES
    ${PROJECT_BINARY_DIR}/lockfree_spsc_queueConfig.cmake
    ${PROJECT_BINARY_DIR}/lockfree_spsc_queueConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_DATADIR}/lockfree_spsc_queue/cmake)
