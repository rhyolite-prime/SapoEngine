# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/emmanueladdo-odame/Documents/cpp_kitchen/sapo-engine/build/_deps/cpr-src")
  file(MAKE_DIRECTORY "/Users/emmanueladdo-odame/Documents/cpp_kitchen/sapo-engine/build/_deps/cpr-src")
endif()
file(MAKE_DIRECTORY
  "/Users/emmanueladdo-odame/Documents/cpp_kitchen/sapo-engine/build/_deps/cpr-build"
  "/Users/emmanueladdo-odame/Documents/cpp_kitchen/sapo-engine/build/_deps/cpr-subbuild/cpr-populate-prefix"
  "/Users/emmanueladdo-odame/Documents/cpp_kitchen/sapo-engine/build/_deps/cpr-subbuild/cpr-populate-prefix/tmp"
  "/Users/emmanueladdo-odame/Documents/cpp_kitchen/sapo-engine/build/_deps/cpr-subbuild/cpr-populate-prefix/src/cpr-populate-stamp"
  "/Users/emmanueladdo-odame/Documents/cpp_kitchen/sapo-engine/build/_deps/cpr-subbuild/cpr-populate-prefix/src"
  "/Users/emmanueladdo-odame/Documents/cpp_kitchen/sapo-engine/build/_deps/cpr-subbuild/cpr-populate-prefix/src/cpr-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/emmanueladdo-odame/Documents/cpp_kitchen/sapo-engine/build/_deps/cpr-subbuild/cpr-populate-prefix/src/cpr-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/emmanueladdo-odame/Documents/cpp_kitchen/sapo-engine/build/_deps/cpr-subbuild/cpr-populate-prefix/src/cpr-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
