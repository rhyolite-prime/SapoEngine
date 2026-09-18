cmake_minimum_required(VERSION 3.28)
project(sapo_engine)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Automated Dependency Fetching
include(FetchContent)
FetchContent_Declare(
        json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# Add CPR (C++ Requests) for HTTP capabilities
set(CPR_ENABLE_TESTS OFF CACHE INTERNAL "")
set(CPR_BUILD_TESTS OFF CACHE INTERNAL "")
set(CPR_GENERATE_COVERAGE OFF CACHE INTERNAL "")
set(CPR_USE_SYSTEM_CURL ON CACHE INTERNAL "")
FetchContent_Declare(cpr GIT_REPOSITORY https://github.com/libcpr/cpr.git GIT_TAG 1.10.5)
FetchContent_MakeAvailable(cpr)

# 1. Define the core interpreter library target
add_library(sapo_core STATIC
        src/parser/WorkflowParser.cpp
        src/runtime/Interpreter.cpp
        src/runtime/VirtualMachine.cpp
        src/runtime/ExpressionEvaluator.cpp
        src/tasks/ActionTask.cpp
        src/tasks/ConditionTask.cpp
        src/tasks/WaitTask.cpp
        src/tasks/QueryTask.cpp
        src/parser/WorkflowParser.hpp
        src/runtime/Interpreter.hpp
        src/runtime/VirtualMachine.hpp
        src/tasks/ConditionTask.hpp
        src/tasks/LoopTask.hpp
        src/tasks/LoopTask.cpp
        src/tasks/ScheduleTask.hpp
        src/tasks/ScheduleTask.cpp
        src/tasks/TransformTask.hpp
        src/tasks/TransformTask.cpp
        src/tasks/ChoiceTask.hpp
        src/tasks/ChoiceTask.cpp
        src/tasks/ParallelTask.hpp
        src/tasks/ParallelTask.cpp
        src/tasks/QueryTask.hpp
        tests/TestRunner.cpp
        src/tasks/CommandTask.hpp
        src/tasks/EventTask.hpp
        src/tasks/WaitTask.hpp
        src/tasks/Subflow.hpp
        src/tasks/Subflow.cpp
        src/tasks/ScriptTask.hpp
        src/tasks/ScriptTask.cpp
        src/tasks/TerminateTask.hpp
        src/tasks/TerminateTask.cpp
        src/tasks/CommandTask.cpp
        src/tasks/EventTask.cpp

)

# 3. Expose the directory containing your headers (.hpp files)
target_include_directories(sapo_core PUBLIC src)

# 4. Define the executable target (using a unique name)
add_executable(sapoc src/main.cpp)

# Link JSON and CPR to your library target securely
target_link_libraries(sapo_core PUBLIC nlohmann_json::nlohmann_json cpr::cpr)

# 5. Link the executable to your core library
target_link_libraries(sapoc PRIVATE sapo_core)


# 5. Tests
# Ensure Catch2 source exists at the path provided
add_executable(sapo_tests
        tests/TestRunner.cpp
        src/third_party/catch2/catch_amalgamated.cpp
)

# Link core library and JSON to the test binary
target_link_libraries(sapo_tests PRIVATE sapo_core nlohmann_json::nlohmann_json)

# Enable testing
enable_testing()
add_test(NAME AllTests COMMAND sapo_tests)

