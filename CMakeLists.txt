cmake_minimum_required(VERSION 3.28)

project(sapo_engine
        VERSION 0.4.0
        DESCRIPTION "Sapo Engine — DSL runtime: parser, interpreter, virtual machine, capability platform"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(SAPO_BUILD_TESTS "Build the Catch2 test suite" ON)
option(SAPO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
# HTTP: the engine ships a NullTransport by default. The cpr wrapper is
# vendored under src/third_party/cpr, so enabling the transport needs no
# network — only system libcurl dev headers (docs/BUILDING.md):
#   Ubuntu/Debian: sudo apt install libcurl4-openssl-dev
option(SAPO_ENABLE_CPR "Build the libcurl-backed HTTP transport (vendored cpr + system libcurl)" OFF)
# Redis: the client is written against POSIX sockets, so it needs no external
# library and no network at build time — only a platform with Berkeley sockets
# (Linux, macOS, *BSD). Off by default to keep the core dependency-free and
# portable; RedisStateStore itself always builds against the IRedisClient seam,
# so a host can inject a hiredis/redis-plus-plus adapter without this option.
option(SAPO_ENABLE_REDIS "Build the POSIX-socket RESP2 client backing RedisStateStore" OFF)
set(SAPO_SANITIZERS "" CACHE STRING "Comma-separated sanitizers, e.g. address,undefined")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Vendored dependencies
#
# The engine builds with no network access: nlohmann/json, Catch2 and cpr are
# vendored under src/third_party/ (exprtk.hpp is kept for reference only — the
# expression language is Sapo Expression Language, see src/runtime/expressions).
# ---------------------------------------------------------------------------
add_library(sapo_vendored INTERFACE)
target_include_directories(sapo_vendored
                           INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/third_party>
                                     $<INSTALL_INTERFACE:include/sapo/third_party>)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
file(GLOB_RECURSE SAPO_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(FILTER SAPO_CORE_SOURCES EXCLUDE REGEX ".*/src/main\\.cpp$")
list(FILTER SAPO_CORE_SOURCES EXCLUDE REGEX ".*/third_party/.*")
if(NOT SAPO_ENABLE_CPR)
    list(FILTER SAPO_CORE_SOURCES EXCLUDE REGEX ".*/http/CprTransport\\.cpp$")
endif()
if(NOT SAPO_ENABLE_REDIS)
    list(FILTER SAPO_CORE_SOURCES EXCLUDE REGEX ".*/redis/SocketRedisClient\\.cpp$")
endif()

add_library(sapo_core STATIC ${SAPO_CORE_SOURCES})
add_library(Sapo::core ALIAS sapo_core)
target_include_directories(sapo_core
                           PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
                                  $<INSTALL_INTERFACE:include/sapo>)
target_link_libraries(sapo_core PUBLIC sapo_vendored Threads::Threads)
target_compile_features(sapo_core PUBLIC cxx_std_23)

if(SAPO_ENABLE_CPR)
    # cpr is vendored (src/third_party/cpr, upstream 1.10.5, BSD-3) and always
    # built against the system libcurl: deterministic version everywhere, no
    # GitHub fetch, no cpr package to install. Pre-check libcurl so the failure
    # message says which package to install instead of cpr's generic one.
    find_package(CURL COMPONENTS HTTP)
    if(NOT CURL_FOUND)
        message(FATAL_ERROR
                "SAPO_ENABLE_CPR=ON requires libcurl development headers:\n"
                "  Ubuntu/Debian: sudo apt install libcurl4-openssl-dev\n"
                "  Fedora:        sudo dnf install libcurl-devel\n"
                "  macOS:         brew install curl")
    endif()
    set(CPR_USE_SYSTEM_CURL ON CACHE BOOL "" FORCE)
    set(CPR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/cpr EXCLUDE_FROM_ALL)
    target_link_libraries(sapo_core PUBLIC cpr::cpr)
    target_compile_definitions(sapo_core PUBLIC SAPO_ENABLE_CPR=1)
endif()

if(SAPO_ENABLE_REDIS)
    if(WIN32)
        message(FATAL_ERROR
                "SAPO_ENABLE_REDIS=ON needs Berkeley sockets (Linux, macOS, *BSD). On Windows, "
                "inject an IRedisClient wrapping hiredis instead — the seam is the same one.")
    endif()
    # PUBLIC so `sapoc` sees the guard around --state-redis, exactly as with CPR.
    target_compile_definitions(sapo_core PUBLIC SAPO_ENABLE_REDIS=1)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(sapo_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
    if(SAPO_WARNINGS_AS_ERRORS)
        target_compile_options(sapo_core PRIVATE -Werror)
    endif()
endif()

if(SAPO_SANITIZERS)
    foreach(san ${SAPO_SANITIZERS})
        target_compile_options(sapo_core PUBLIC -fsanitize=${san} -fno-omit-frame-pointer -g)
        target_link_options(sapo_core PUBLIC -fsanitize=${san})
    endforeach()
endif()

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
add_executable(sapoc src/main.cpp)
target_link_libraries(sapoc PRIVATE sapo_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(sapoc PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# Tests (Catch2 amalgamated; one ctest case per suite file)
# ---------------------------------------------------------------------------
if(SAPO_BUILD_TESTS)
    enable_testing()

    add_library(sapo_catch_main OBJECT src/third_party/catch2/catch_amalgamated.cpp)
    target_link_libraries(sapo_catch_main PUBLIC sapo_vendored)
    target_include_directories(sapo_catch_main PUBLIC src)
    # The amalgamated file is a compile-time monster; keep it cheap even in Debug.
    target_compile_options(sapo_catch_main PRIVATE -O1)

    file(GLOB SAPO_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp)
    foreach(test_source ${SAPO_TEST_SOURCES})
        get_filename_component(test_name ${test_source} NAME_WE)
        add_executable(${test_name} ${test_source} $<TARGET_OBJECTS:sapo_catch_main>)
        target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_link_libraries(${test_name} PRIVATE sapo_core sapo_catch_main)
        add_test(NAME ${test_name} COMMAND ${test_name})
        set_tests_properties(${test_name} PROPERTIES
                             ENVIRONMENT "SAPO_BLUEPRINT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/examples")
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Install (docs/INTEGRATING.md: find_package(SapoEngine) + libsapo_core.a)
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS sapo_core sapoc sapo_vendored
        EXPORT SapoEngineTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/sapo
        FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/schemas/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/sapo
        FILES_MATCHING PATTERN "*.json")
install(EXPORT SapoEngineTargets
        FILE SapoEngineTargets.cmake
        NAMESPACE Sapo::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SapoEngine)
write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/SapoEngineConfigVersion.cmake
        COMPATIBILITY SameMajorVersion)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/SapoEngineConfig.cmake
     "include(\"\${CMAKE_CURRENT_LIST_DIR}/SapoEngineTargets.cmake\")\n")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/SapoEngineConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/SapoEngineConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SapoEngine)
