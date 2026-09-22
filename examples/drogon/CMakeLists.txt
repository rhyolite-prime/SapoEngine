cmake_minimum_required(VERSION 3.28)

project(sapo_drogon_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find Drogon Web Framework
find_package(Drogon REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Link Sapo Engine
#
# Option A: In-tree / submodule consumption (Recommended):
#   add_subdirectory(../.. sapo-engine EXCLUDE_FROM_ALL)
#   Provides the Sapo::core target directly.
#
# Option B: System or prefix installed package:
#   find_package(SapoEngine REQUIRED PATHS /opt/sapo)
# ---------------------------------------------------------------------------
if(NOT TARGET Sapo::core)
    find_package(SapoEngine QUIET PATHS /opt/sapo /usr/local)
    if(NOT SapoEngine_FOUND)
        message(STATUS "[Sapo Drogon] SapoEngine package not installed. Linking from parent source tree...")
        set(SAPO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(SAPO_ENABLE_CPR OFF CACHE BOOL "" FORCE) # We use DrogonHttpTransport instead!
        set(SAPO_ENABLE_REDIS ON CACHE BOOL "" FORCE)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." sapo-engine EXCLUDE_FROM_ALL)
    endif()
endif()

# Application Executable
add_executable(sapo_drogon_service
    main.cc
    controllers/UssdWorkflowController.cc
    adapters/DrogonHttpTransport.cc
)

target_include_directories(sapo_drogon_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/controllers
    ${CMAKE_CURRENT_SOURCE_DIR}/adapters
)

target_link_libraries(sapo_drogon_service PRIVATE
    Drogon::Drogon
    Sapo::core
    Threads::Threads
)
