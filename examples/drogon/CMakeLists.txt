# sapo_service — reference Drogon front-end for Sapo Engine (service mode).
#
# Standalone project: it builds the engine from its source tree and links the
# static core library into a Drogon binary.
#
#   cmake -S examples/drogon -B build-service
#   cmake --build build-service -j
#   ./build-service/sapo_service --port 8090 --workflows examples --state-dir .sapo-state
#
# Requirements: CMake >= 3.28, a C++23 compiler (GCC >= 13 / Clang >= 17),
# Drogon >= 1.9 (find_package), libcurl dev headers (HTTP nodes in blueprints).
cmake_minimum_required(VERSION 3.28)

project(sapo_drogon_service
        VERSION 0.1.0
        DESCRIPTION "Sapo Engine served over HTTP with Drogon"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Drogon: system package, vcpkg, or -DCMAKE_PREFIX_PATH=<drogon install prefix>.
find_package(Drogon REQUIRED)

# The engine: build it from its checkout and link Sapo::core.
# SAPO_ENABLE_CPR gives blueprints outbound HTTP via libcurl; set it OFF and
# inject your own sapo::http::IHttpTransport instead if you prefer one stack.
set(SAPO_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
    "Path to the SapoEngine checkout")
set(SAPO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SAPO_ENABLE_CPR ON CACHE BOOL "" FORCE)
add_subdirectory("${SAPO_ENGINE_DIR}" sapo-engine EXCLUDE_FROM_ALL)

add_executable(sapo_service main.cc)
target_link_libraries(sapo_service PRIVATE Sapo::core Drogon::Drogon)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(sapo_service PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
