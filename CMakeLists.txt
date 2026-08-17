cmake_minimum_required(VERSION 3.14)
project(SimplePluginExample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Qt packages
# Network is needed explicitly by the token-exchange tests (QLocalServer /
# QLocalSocket); it is also pulled transitively by RemoteObjects in the
# runtime build, but we declare it here so the test target can link it
# explicitly without depending on that transitive resolution.
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network RemoteObjects)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network RemoteObjects)

# Find nlohmann_json
find_package(nlohmann_json REQUIRED)

# OpenSSL needed transitively by logos_sdk (plain-C++ TCP+SSL transport)
find_package(OpenSSL REQUIRED)

# Find spdlog
find_package(spdlog REQUIRED)

# Set output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Add external library paths to build RPATH so test binaries can find them
if(DEFINED LOGOS_PACKAGE_MANAGER_ROOT)
    list(APPEND CMAKE_BUILD_RPATH "${LOGOS_PACKAGE_MANAGER_ROOT}/lib")
endif()

# Build src first to ensure logos_core is built before modules
add_subdirectory(src)

# Tests. Gated EXPLICITLY rather than left to find_package: the suite is
# POSIX-only (tests/test_process_stats.cpp uses <spawn.h>, <sys/wait.h>,
# posix_spawn/waitpid/kill and `environ`; tests/test_subprocess_manager.cpp
# spawns /bin/sh), none of which mingw-w64 provides. The default build target
# is `all`, so an ungated tests/ subdirectory would be compiled by every
# cross build.
#
# The gate wraps the GTest lookup too: dropping only `add_subdirectory(tests)`
# would still leave find_package(GTest) able to miss and fall through to the
# FetchContent download, which dies in the nix sandbox with a misleading
# "downloading ... failed".
option(LOGOS_BUILD_TESTS "Build the logos_core test suite" ON)
if(WIN32)
    set(LOGOS_BUILD_TESTS OFF)
endif()

if(LOGOS_BUILD_TESTS)
    # Try to find GoogleTest via find_package first (for Nix and system installations)
    find_package(GTest QUIET)

    if(NOT GTest_FOUND)
        # Fall back to FetchContent if GoogleTest is not found
        message(STATUS "GoogleTest not found via find_package, using FetchContent")
        include(FetchContent)
        FetchContent_Declare(googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        )
        FetchContent_MakeAvailable(googletest)
    else()
        message(STATUS "Using system GoogleTest")
    endif()

    enable_testing()
    include(GoogleTest)

    # Add tests subdirectory
    add_subdirectory(tests)
endif()

# Install rules. The logos_host_qt binary (and its logos_host symlink) now live
# in logos-module-loader-qt; liblogos installs only the core library here, and
# bin.nix re-exports the host binary from that package so frontends are
# unaffected.
# RUNTIME DESTINATION matters on Windows: a DLL is a RUNTIME artifact, and with
# only LIBRARY/ARCHIVE named CMake sends it to the DEFAULT runtime destination
# (bin) while nix/lib.nix only ever copies ${build}/lib -- which would yield a
# lib output holding the import library and no DLL, silently. Pointing RUNTIME
# at lib keeps the output shape identical on all three platforms.
install(TARGETS logos_core
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION lib
)

# Install headers
install(FILES src/logos_core/logos_core.h
    DESTINATION include
)
