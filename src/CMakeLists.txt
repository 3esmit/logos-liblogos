set(CMAKE_AUTOMOC ON)

set(_default_sdk_root "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/logos-cpp-sdk")
if(NOT DEFINED LOGOS_CPP_SDK_ROOT OR LOGOS_CPP_SDK_ROOT STREQUAL "")
  set(LOGOS_CPP_SDK_ROOT "${_default_sdk_root}")
endif()

get_filename_component(LOGOS_CPP_SDK_ROOT "${LOGOS_CPP_SDK_ROOT}" ABSOLUTE)

# The SDK stack since the Qt split: logos-protocol (transports + lp_* C
# ABI), logos-qt-sdk (LogosAPI + provider glue — what this runtime's Qt
# side uses), logos-cpp-sdk (Qt-free std headers). The `logos_sdk` alias
# is kept so the rest of this file (and the host targets) stay unchanged;
# it now points at logos-qt-sdk, whose link interface chains
# logos-protocol and its OpenSSL/Boost/nlohmann deps.
if(NOT DEFINED LOGOS_PROTOCOL_ROOT AND DEFINED ENV{LOGOS_PROTOCOL_ROOT})
  set(LOGOS_PROTOCOL_ROOT "$ENV{LOGOS_PROTOCOL_ROOT}")
endif()
if(NOT DEFINED LOGOS_QT_SDK_ROOT AND DEFINED ENV{LOGOS_QT_SDK_ROOT})
  set(LOGOS_QT_SDK_ROOT "$ENV{LOGOS_QT_SDK_ROOT}")
endif()

if(EXISTS "${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol"
   AND EXISTS "${LOGOS_QT_SDK_ROOT}/lib/cmake/logos-qt-sdk")
  message(STATUS "Using logos-protocol at ${LOGOS_PROTOCOL_ROOT}")
  message(STATUS "Using logos-qt-sdk at ${LOGOS_QT_SDK_ROOT}")
  find_package(logos-protocol REQUIRED
    PATHS "${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol" NO_DEFAULT_PATH)
  find_package(logos-qt-sdk REQUIRED
    PATHS "${LOGOS_QT_SDK_ROOT}/lib/cmake/logos-qt-sdk" NO_DEFAULT_PATH)
  if(NOT TARGET logos_sdk)
    add_library(logos_sdk INTERFACE IMPORTED)
    target_link_libraries(logos_sdk INTERFACE logos-qt-sdk::logos_qt_sdk)
  endif()
else()
  message(FATAL_ERROR "logos-protocol / logos-qt-sdk not found. Set "
                      "LOGOS_PROTOCOL_ROOT and LOGOS_QT_SDK_ROOT to built packages.")
endif()

# Qt-free base SDK headers (logos_module_context.h etc.) — header-only.
if(EXISTS "${LOGOS_CPP_SDK_ROOT}/lib/cmake/logos-cpp-sdk")
  find_package(logos-cpp-sdk REQUIRED
    PATHS "${LOGOS_CPP_SDK_ROOT}/lib/cmake/logos-cpp-sdk" NO_DEFAULT_PATH)
  if(NOT TARGET logos_headers)
    add_library(logos_headers INTERFACE IMPORTED)
    target_link_libraries(logos_headers INTERFACE logos-cpp-sdk::logos_headers)
  endif()
  target_link_libraries(logos_sdk INTERFACE logos-cpp-sdk::logos_headers)
endif()

# Find logos-module library
if(DEFINED LOGOS_MODULE_ROOT AND EXISTS "${LOGOS_MODULE_ROOT}/lib")
  message(STATUS "Using logos-module at ${LOGOS_MODULE_ROOT}")
  
  find_library(LOGOS_MODULE_LIBRARY
    NAMES logos_module
    PATHS "${LOGOS_MODULE_ROOT}/lib"
    NO_DEFAULT_PATH
  )
  
  if(NOT LOGOS_MODULE_LIBRARY)
    message(FATAL_ERROR "logos_module library not found in ${LOGOS_MODULE_ROOT}/lib")
  endif()
  
  # Create an imported target for logos_module
  add_library(logos_module STATIC IMPORTED)
  set_target_properties(logos_module PROPERTIES
    IMPORTED_LOCATION "${LOGOS_MODULE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_MODULE_ROOT}/include"
  )
else()
  message(STATUS "LOGOS_MODULE_ROOT not set, logos-module will need to be provided")
endif()

# Find process-stats library
if(DEFINED PROCESS_STATS_ROOT AND EXISTS "${PROCESS_STATS_ROOT}/lib")
  message(STATUS "Using process-stats at ${PROCESS_STATS_ROOT}")

  find_library(PROCESS_STATS_LIBRARY
    NAMES process_stats
    PATHS "${PROCESS_STATS_ROOT}/lib"
    NO_DEFAULT_PATH
  )

  if(NOT PROCESS_STATS_LIBRARY)
    message(FATAL_ERROR "process_stats library not found in ${PROCESS_STATS_ROOT}/lib")
  endif()

  add_library(process_stats STATIC IMPORTED)
  set_target_properties(process_stats PROPERTIES
    IMPORTED_LOCATION "${PROCESS_STATS_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PROCESS_STATS_ROOT}/include"
  )
else()
  message(STATUS "PROCESS_STATS_ROOT not set, process-stats will need to be provided")
endif()

# Find logos-container (the Qt-free, header-only container contract:
# ModuleContainer interface + ModuleDescriptor/LoadedModuleHandle + the
# module-name allowlist). Header-only, so it is an INTERFACE import keyed on
# its include dir rather than a library.
if(DEFINED LOGOS_CONTAINER_ROOT AND EXISTS "${LOGOS_CONTAINER_ROOT}/include")
  message(STATUS "Using logos-container at ${LOGOS_CONTAINER_ROOT}")

  add_library(logos_container INTERFACE IMPORTED GLOBAL)
  set_target_properties(logos_container PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_CONTAINER_ROOT}/include"
    INTERFACE_LINK_LIBRARIES "nlohmann_json::nlohmann_json"
  )
else()
  message(FATAL_ERROR "LOGOS_CONTAINER_ROOT not set or ${LOGOS_CONTAINER_ROOT}/include does not exist")
endif()

# Container implementation — selected by the BUILD, not named here.
#
# The chosen implementation package ships a generic CMake config package
# (LogosContainerImpl) that defines the imported target LogosContainerImpl::impl,
# carrying its static library AND its own transitive deps (Boost, spdlog, ...).
# liblogos just asks for "the container implementation" by that generic name and
# links the target — it knows neither the library name nor the dependencies. nix
# puts the chosen package on CMAKE_PREFIX_PATH (default logos-container-subprocess);
# switching to another container (Docker, ...) means a package that provides the
# same config — no edit to this file, the nix flags, or any C++.
find_package(LogosContainerImpl REQUIRED)

# Find logos-module-loader (the header-only ModuleFormatLoader contract).
if(DEFINED LOGOS_MODULE_LOADER_ROOT AND EXISTS "${LOGOS_MODULE_LOADER_ROOT}/include")
  message(STATUS "Using logos-module-loader at ${LOGOS_MODULE_LOADER_ROOT}")

  add_library(logos_module_loader INTERFACE IMPORTED GLOBAL)
  set_target_properties(logos_module_loader PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_MODULE_LOADER_ROOT}/include"
    INTERFACE_LINK_LIBRARIES "logos_container"
  )
else()
  message(FATAL_ERROR "LOGOS_MODULE_LOADER_ROOT not set or ${LOGOS_MODULE_LOADER_ROOT}/include does not exist")
endif()

# Format-loader implementation — selected by the BUILD (mirrors the container).
# The chosen package ships a generic CMake config package (LogosFormatLoaderImpl)
# defining the imported target LogosFormatLoaderImpl::impl with its own deps.
# Default: logos-module-loader-qt (which also ships the logos_host_qt binary,
# re-exported via bin.nix — independent of this lib link).
find_package(LogosFormatLoaderImpl REQUIRED)


# Define the library sources.
# The container contract (ModuleContainer + descriptor value types) lives in
# logos-container and the ModuleFormatLoader contract in logos-module-loader.
# The concrete container + format-loader implementations are NOT named here:
# they are linked through the build-selected `LogosContainerImpl::impl` /
# `LogosFormatLoaderImpl::impl` targets found via find_package above (default
# subprocess + qt, chosen in nix/default.nix). The ModuleLoader base, the
# composite/registry orchestration, and the logging foundation are core concerns
# and stay here.
set(LOGOS_CORE_SOURCES
    logging/logos_log.cpp
    logging/logos_log.h
    logos_core/logos_core.cpp
    logos_core/logos_core.h
    logos_core/module_registry.cpp
    logos_core/module_registry.h
    logos_core/dependency_resolver.cpp
    logos_core/dependency_resolver.h
    logos_core/access_policy.cpp
    logos_core/access_policy.h
    logos_core/module_manager.cpp
    logos_core/module_manager.h
    logos_core/module_loader.h
    logos_core/composite_module_loader.cpp
    logos_core/composite_module_loader.h
    logos_core/module_loader_registry.cpp
    logos_core/module_loader_registry.h
)

# The format-loader implementation and the logos_host_qt host binary live in the
# package the build selects (default logos-module-loader-qt); the
# ModuleFormatLoader interface lives in logos-module-loader. liblogos links the
# implementation lib via the find_package'd `LogosFormatLoaderImpl::impl` target
# and re-exports the host binary from the same package via bin.nix.

# Create the logos core library
add_library(logos_core SHARED ${LOGOS_CORE_SOURCES})

# Set the LOGOS_CORE_LIBRARY definition for the library
target_compile_definitions(logos_core PRIVATE LOGOS_CORE_LIBRARY)

# PE export table: nothing to do here. LOGOS_CORE_EXPORT now expands to
# __declspec(dllexport) on Windows (see logos_core/logos_core.h), which both
# exports the C API and, because GNU ld disables auto-export image-wide once any
# symbol is dllexported, stops the internal C++ surface leaking.
#
# This deliberately REPLACES an earlier `-Wl,--export-all-symbols`. That did
# export the C API, but it also exported all 13,252 symbols in the image --
# LogosAPI's among them -- so any consumer linking both liblogos_core and the
# qt-sdk static library failed with "multiple definition of `LogosAPI::LogosAPI'".
# Do not reinstate it; if the C API ever stops being exported, the cause is a
# missing LOGOS_CORE_EXPORT on a declaration, not a missing linker flag.
#
# The C API is not the whole story, though. Narrowing the export table to it was
# what made main_ui link again, but it also left every in-process image with its
# own statically linked copy of the shared C++ runtime -- and therefore its own
# TokenManager singleton, so a capability token saved by the host was invisible
# to the UI plugin and every cross-module call was refused. ELF and Mach-O
# interpose these symbols across the process and get one instance for free; PE
# does not. The block below closes that gap the only way PE allows: liblogos_core
# publishes the shared types EXPLICITLY, and the in-process consumers import them
# instead of re-linking them. Neither horn of the old dilemma -- export
# everything and collide, or export nothing and duplicate -- is taken.
#
# Full rationale, including why the consumer-side __declspec(dllimport) is the
# load-bearing half rather than the export, lives in
# logos-protocol/cpp/logos_shared_api.h.
if(WIN32 AND TARGET logos-protocol::logos_protocol AND TARGET logos-qt-sdk::logos_qt_sdk)
    # Ask the imported targets where their archives actually are rather than
    # rebuilding the path by hand, so a rename or a layout change fails at
    # generate time instead of producing an empty export list.
    set(_logos_protocol_archive "$<TARGET_FILE:logos-protocol::logos_protocol>")
    set(_logos_qt_sdk_archive   "$<TARGET_FILE:logos-qt-sdk::logos_qt_sdk>")

    # --whole-archive, i.e. the whole of both libraries goes into the DLL even
    # where liblogos itself never calls it.
    #
    # This is not gold-plating; it is what makes "single provider" true rather
    # than approximate. The consumers link an EMPTY stand-in for these archives
    # (logos-basecamp/cmake/LogosSharedFromDll.cmake) because ld would otherwise
    # pull members for incidental reasons -- it was observed pulling
    # logos_api.cpp.obj to satisfy std::string's move constructor -- and each
    # pulled object collides with our export. Once the consumer has no archive to
    # fall back on, anything the DLL failed to include becomes an undefined
    # reference in a downstream repo. Linking normally would include only the
    # objects liblogos happens to reference, which is a moving target.
    target_link_options(logos_core PRIVATE
        "-Wl,--whole-archive"
        "${_logos_protocol_archive}"
        "${_logos_qt_sdk_archive}"
        "-Wl,--no-whole-archive")

    # CMAKE_NM is normally set by the toolchain file; fall back to the
    # cross-prefixed binary so a plain `cmake -DCMAKE_TOOLCHAIN_FILE=...` still
    # generates. A missing nm must fail here, loudly, rather than silently
    # emitting an empty .def and restoring the split-brain.
    set(_logos_nm "${CMAKE_NM}")
    if(NOT _logos_nm)
        find_program(_logos_nm NAMES "${CMAKE_CXX_COMPILER_TARGET}-nm" x86_64-w64-mingw32-nm nm)
    endif()
    if(NOT _logos_nm)
        message(FATAL_ERROR "nm not found; cannot generate the liblogos_core export definition")
    endif()

    set(_logos_shared_def "${CMAKE_CURRENT_BINARY_DIR}/logos_core_shared_exports.def")
    add_custom_command(
        OUTPUT "${_logos_shared_def}"
        COMMAND ${CMAKE_COMMAND} -E env sh
                "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/gen-shared-exports.sh"
                "${_logos_nm}" "${_logos_shared_def}"
                "${_logos_protocol_archive}" "*"
                "${_logos_qt_sdk_archive}"   "*"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/gen-shared-exports.sh"
                "${_logos_protocol_archive}" "${_logos_qt_sdk_archive}"
        COMMENT "Generating liblogos_core shared-runtime export definition"
        VERBATIM
    )
    add_custom_target(logos_core_shared_exports DEPENDS "${_logos_shared_def}")
    add_dependencies(logos_core logos_core_shared_exports)

    # A .def passed to the link ADDS to the export table; the ~18
    # __declspec(dllexport) logos_core_* C API entries survive alongside it.
    target_link_options(logos_core PRIVATE "${_logos_shared_def}")
    set_property(TARGET logos_core APPEND PROPERTY LINK_DEPENDS "${_logos_shared_def}")
endif()

# Portable build: selects portable LGX variants instead of dev variants
option(LOGOS_PORTABLE_BUILD "Build for portable variant selection" OFF)
if(LOGOS_PORTABLE_BUILD)
    target_compile_definitions(logos_core PRIVATE LOGOS_PORTABLE_BUILD=1)
endif()

# Find Boost (header-only usage + Boost.Process/Asio)
find_package(Boost REQUIRED COMPONENTS process filesystem)

# Find logos-package-manager library
if(DEFINED LOGOS_PACKAGE_MANAGER_ROOT AND EXISTS "${LOGOS_PACKAGE_MANAGER_ROOT}/lib")
  message(STATUS "Using logos-package-manager at ${LOGOS_PACKAGE_MANAGER_ROOT}")

  find_library(LOGOS_PACKAGE_MANAGER_LIBRARY
    NAMES package_manager_lib
    PATHS "${LOGOS_PACKAGE_MANAGER_ROOT}/lib"
    NO_DEFAULT_PATH
  )

  if(NOT LOGOS_PACKAGE_MANAGER_LIBRARY)
    message(FATAL_ERROR "package_manager_lib not found in ${LOGOS_PACKAGE_MANAGER_ROOT}/lib")
  endif()

  add_library(package_manager_lib SHARED IMPORTED)
  set_target_properties(package_manager_lib PROPERTIES
    IMPORTED_LOCATION "${LOGOS_PACKAGE_MANAGER_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_PACKAGE_MANAGER_ROOT}/include"
  )
  # On Windows a SHARED IMPORTED target links through its IMPORT library, and
  # CMake hard-errors at generate time when IMPORTED_IMPLIB is unset. find_library
  # returns libpackage_manager_lib.dll.a there (CMAKE_FIND_LIBRARY_SUFFIXES for
  # MinGW is .dll.a;.a), which is exactly the import library to link.
  if(WIN32)
    set_target_properties(package_manager_lib PROPERTIES
      IMPORTED_IMPLIB "${LOGOS_PACKAGE_MANAGER_LIBRARY}"
    )
  endif()
else()
  message(FATAL_ERROR "LOGOS_PACKAGE_MANAGER_ROOT not set or ${LOGOS_PACKAGE_MANAGER_ROOT}/lib does not exist")
endif()

# Link Qt libraries, SDK, module library, and package-manager library.
# The container/format-loader implementations are linked through the generic,
# build-selected `LogosContainerImpl::impl` / `LogosFormatLoaderImpl::impl`
# targets (each comes from a find_package config that carries its own deps);
# `logos_container` is the shared contract. liblogos names no specific
# implementation here.
target_link_libraries(logos_core PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
    logos_sdk
    logos_module
    process_stats
    logos_container
    LogosContainerImpl::impl
    LogosFormatLoaderImpl::impl
    package_manager_lib
    nlohmann_json::nlohmann_json
    Boost::process
    spdlog::spdlog
)

# Include directories for the library
target_include_directories(logos_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/logos_core
    ${CMAKE_CURRENT_SOURCE_DIR}/module_loaders
    ${Qt${QT_VERSION_MAJOR}_INCLUDE_DIRS}
)

# Add logos_module include directory if available
if(DEFINED LOGOS_MODULE_ROOT AND EXISTS "${LOGOS_MODULE_ROOT}/include")
  target_include_directories(logos_core PRIVATE
    ${LOGOS_MODULE_ROOT}/include
    ${LOGOS_MODULE_ROOT}/include/module_lib
  )
endif()

# Add process-stats include directory if available
if(DEFINED PROCESS_STATS_ROOT AND EXISTS "${PROCESS_STATS_ROOT}/include")
  target_include_directories(logos_core PRIVATE ${PROCESS_STATS_ROOT}/include)
endif()


# Add SDK include directory if using built SDK
if(EXISTS "${LOGOS_CPP_SDK_ROOT}/lib" AND EXISTS "${LOGOS_CPP_SDK_ROOT}/include")
  target_include_directories(logos_core PRIVATE 
    ${LOGOS_CPP_SDK_ROOT}/include
    ${LOGOS_CPP_SDK_ROOT}/include/cpp
    ${LOGOS_CPP_SDK_ROOT}/include/core
  )
else()
  target_include_directories(logos_core PRIVATE ${LOGOS_CPP_SDK_ROOT}/cpp)
endif()

# Set RPATH settings for the library
if(APPLE)
    set(_qt_runtime_paths "@loader_path")
    if(DEFINED Qt${QT_VERSION_MAJOR}_DIR)
        get_filename_component(_qt_lib_dir "${Qt${QT_VERSION_MAJOR}_DIR}/../.." REALPATH)
        list(APPEND _qt_runtime_paths "${_qt_lib_dir}")
    endif()
    list(REMOVE_DUPLICATES _qt_runtime_paths)

    set_target_properties(logos_core PROPERTIES
        INSTALL_RPATH "${_qt_runtime_paths}"
        INSTALL_NAME_DIR "@rpath"
        BUILD_WITH_INSTALL_NAME_DIR TRUE
        BUILD_WITH_INSTALL_RPATH TRUE)
else()
    set_target_properties(logos_core PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH TRUE)
endif()
