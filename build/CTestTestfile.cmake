# CMake generated Testfile for 
# Source directory: /Users/emmanueladdo-odame/Documents/cpp_kitchen/sapo-engine
# Build directory: /Users/emmanueladdo-odame/Documents/cpp_kitchen/sapo-engine/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[AllTests]=] "/Users/emmanueladdo-odame/Documents/cpp_kitchen/sapo-engine/build/sapo_tests")
set_tests_properties([=[AllTests]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/emmanueladdo-odame/Documents/cpp_kitchen/sapo-engine/CMakeLists.txt;88;add_test;/Users/emmanueladdo-odame/Documents/cpp_kitchen/sapo-engine/CMakeLists.txt;0;")
subdirs("_deps/json-build")
subdirs("_deps/cpr-build")
