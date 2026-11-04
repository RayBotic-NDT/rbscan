cmake_minimum_required(VERSION 3.24)

project(rbscan
    VERSION 0.3.0
    DESCRIPTION "RayBotic scan data format: HDF5 master file + segmented sensor streams"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

option(RBSCAN_BUILD_TESTS "Build the C++ unit tests" ${PROJECT_IS_TOP_LEVEL})
option(RBSCAN_BUILD_PYTHON "Build the Python extension module" ${PROJECT_IS_TOP_LEVEL})
option(RBSCAN_BUILD_EXAMPLES "Build the C++ examples" ${PROJECT_IS_TOP_LEVEL})
option(RBSCAN_BUILD_TOOLS "Build the rbscan command line tool" ON)
option(RBSCAN_INSTALL "Generate install rules (off when used as a subproject)" ${PROJECT_IS_TOP_LEVEL})
option(RBSCAN_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# Everything lands next to each other, so rbscan.dll / hdf5.dll are found by
# tests and examples without touching PATH.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

include(rbscan_warnings)
include(rbscan_deps)
include(rbscan_embed_schemas)

add_subdirectory(src)

if(RBSCAN_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(RBSCAN_BUILD_PYTHON)
    add_subdirectory(python)
endif()

if(RBSCAN_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(RBSCAN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
