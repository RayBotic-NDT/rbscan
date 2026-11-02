# Third-party dependencies.
#
# HDF5 (+ zlib for its deflate filter) is built once at configure time into
# RBSCAN_DEPS_DIR and reused by every build tree, because HDF5 does not work
# well as a CMake subproject. It is always a Release shared library: rbscan only
# uses its C API, so Debug builds can link the Release DLL. The DLL is called
# rbscan_hdf5.dll: Windows loads one module per file name and process, so a
# plain hdf5.dll would clash with the one h5py (or MATLAB, ...) brings along.
#
# An existing HDF5 install can be used instead: point HDF5_DIR / CMAKE_PREFIX_PATH
# at it and set RBSCAN_BUILD_HDF5=OFF.
#
# libdeflate does the parallel chunk compression (zlib-format streams, which the
# HDF5 deflate filter reads), LZ4 the Bitshuffle+LZ4 codec, nlohmann_json parses
# the schemas. All are small and come in via FetchContent.

include(FetchContent)

set(RBSCAN_HDF5_VERSION 1.14.6)
set(RBSCAN_ZLIB_VERSION 1.3.1)

option(RBSCAN_BUILD_HDF5 "Build HDF5 from source into RBSCAN_DEPS_DIR" ON)
set(RBSCAN_DEPS_DIR "${PROJECT_SOURCE_DIR}/.deps" CACHE PATH
    "Where the HDF5 superbuild puts its sources, build trees and install")

function(_rbscan_run_step)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "rbscan dependency build failed:\n${ARGN}\n${_out}")
    endif()
endfunction()

function(_rbscan_build_hdf5 prefix)
    set(_src "${RBSCAN_DEPS_DIR}/src")
    set(_bld "${RBSCAN_DEPS_DIR}/build")
    set(_gen -G "${CMAKE_GENERATOR}")
    if(CMAKE_GENERATOR_PLATFORM)
        list(APPEND _gen -A "${CMAKE_GENERATOR_PLATFORM}")
    elseif(CMAKE_GENERATOR MATCHES "Visual Studio")
        list(APPEND _gen -A x64)
    endif()
    set(_common
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX=${prefix}
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        # zlib 1.3.1 still declares cmake_minimum_required(2.4.4), which CMake 4 rejects.
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5)

    # Sources only (SOURCE_SUBDIR points nowhere, so nothing is added here).
    FetchContent_Declare(rbscan_zlib_src
        URL https://github.com/madler/zlib/releases/download/v${RBSCAN_ZLIB_VERSION}/zlib-${RBSCAN_ZLIB_VERSION}.tar.gz
        SOURCE_DIR "${_src}/zlib-${RBSCAN_ZLIB_VERSION}"
        SOURCE_SUBDIR _none)
    FetchContent_Declare(rbscan_hdf5_src
        GIT_REPOSITORY https://github.com/HDFGroup/hdf5.git
        GIT_TAG hdf5_${RBSCAN_HDF5_VERSION}
        GIT_SHALLOW TRUE
        SOURCE_DIR "${_src}/hdf5-${RBSCAN_HDF5_VERSION}"
        SOURCE_SUBDIR _none)
    FetchContent_MakeAvailable(rbscan_zlib_src rbscan_hdf5_src)

    message(STATUS "rbscan: building zlib ${RBSCAN_ZLIB_VERSION} (once)")
    _rbscan_run_step(${CMAKE_COMMAND} -S "${_src}/zlib-${RBSCAN_ZLIB_VERSION}"
        -B "${_bld}/zlib" ${_gen} ${_common} -DZLIB_BUILD_EXAMPLES=OFF)
    _rbscan_run_step(${CMAKE_COMMAND} --build "${_bld}/zlib" --config Release
        --target install --parallel)

    if(WIN32)
        set(_zlib_lib "${prefix}/lib/zlibstatic.lib")
    else()
        set(_zlib_lib "${prefix}/lib/libz.a")
    endif()

    message(STATUS "rbscan: building HDF5 ${RBSCAN_HDF5_VERSION} (once, takes a few minutes)")
    _rbscan_run_step(${CMAKE_COMMAND} -S "${_src}/hdf5-${RBSCAN_HDF5_VERSION}"
        -B "${_bld}/rbscan_hdf5" ${_gen} ${_common}
        -DBUILD_SHARED_LIBS=ON
        -DBUILD_STATIC_LIBS=OFF
        -DHDF5_EXTERNAL_LIB_PREFIX=rbscan_
        -DBUILD_TESTING=OFF
        -DHDF5_BUILD_TOOLS=ON
        -DHDF5_BUILD_UTILS=OFF
        -DHDF5_BUILD_EXAMPLES=OFF
        -DHDF5_BUILD_CPP_LIB=OFF
        -DHDF5_BUILD_HL_LIB=OFF
        -DHDF5_BUILD_FORTRAN=OFF
        -DHDF5_BUILD_JAVA=OFF
        -DHDF5_ENABLE_SZIP_SUPPORT=OFF
        -DHDF5_ENABLE_Z_LIB_SUPPORT=ON
        -DZLIB_USE_EXTERNAL=OFF
        -DZLIB_INCLUDE_DIR=${prefix}/include
        -DZLIB_LIBRARY=${_zlib_lib}
        -DZLIB_ROOT=${prefix})
    _rbscan_run_step(${CMAKE_COMMAND} --build "${_bld}/rbscan_hdf5" --config Release
        --target install --parallel)
endfunction()

if(RBSCAN_BUILD_HDF5)
    set(_prefix "${RBSCAN_DEPS_DIR}/install/rbscan_hdf5-${RBSCAN_HDF5_VERSION}")
    if(NOT EXISTS "${_prefix}/.complete")
        _rbscan_build_hdf5("${_prefix}")
        file(TOUCH "${_prefix}/.complete")
    endif()
    set(RBSCAN_HDF5_PREFIX "${_prefix}" CACHE INTERNAL "")
    # GLOBAL: a parent project that adds rbscan as a subdirectory needs the
    # target to ship the DLL.
    find_package(HDF5 CONFIG REQUIRED NAMES hdf5 GLOBAL
        PATHS "${_prefix}" "${_prefix}/cmake" "${_prefix}/share/cmake" NO_DEFAULT_PATH)
else()
    find_package(HDF5 CONFIG REQUIRED NAMES hdf5 GLOBAL)
endif()

foreach(_t hdf5::hdf5-shared hdf5-shared hdf5::rbscan_hdf5-shared rbscan_hdf5-shared)
    if(TARGET ${_t})
        set(RBSCAN_HDF5_TARGET ${_t})
        break()
    endif()
endforeach()
if(NOT RBSCAN_HDF5_TARGET)
    message(FATAL_ERROR "rbscan: HDF5 found, but without a shared C library target")
endif()
set(RBSCAN_HDF5_TARGET "${RBSCAN_HDF5_TARGET}" CACHE INTERNAL "HDF5 target rbscan links")

# The HDF5 runtime library to ship, and the name to ship it under: on Linux the
# soname (librbscan_hdf5.so.310), which is what the loader looks for, so no
# symlinks are needed in the SDK or the wheel.
foreach(_cfg _RELEASE _NOCONFIG "")
    get_target_property(_loc ${RBSCAN_HDF5_TARGET} IMPORTED_LOCATION${_cfg})
    if(_loc)
        get_target_property(_soname ${RBSCAN_HDF5_TARGET} IMPORTED_SONAME${_cfg})
        break()
    endif()
endforeach()
if(NOT _loc)
    message(FATAL_ERROR "rbscan: no runtime library location for ${RBSCAN_HDF5_TARGET}")
endif()
set(RBSCAN_HDF5_RUNTIME_FILE "${_loc}" CACHE INTERNAL "")
if(_soname AND NOT WIN32)
    get_filename_component(_name "${_soname}" NAME)
else()
    get_filename_component(_name "${_loc}" NAME)
endif()
set(RBSCAN_HDF5_RUNTIME_NAME "${_name}" CACHE INTERNAL "")
message(STATUS "rbscan: HDF5 ${HDF5_VERSION} (${RBSCAN_HDF5_TARGET})")

FetchContent_Declare(libdeflate
    GIT_REPOSITORY https://github.com/ebiggers/libdeflate.git
    GIT_TAG v1.24
    GIT_SHALLOW TRUE)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
set(JSON_Install OFF CACHE BOOL "" FORCE)

# Only lz4.c is needed; its own CMake project lives in build/cmake.
FetchContent_Declare(rbscan_lz4
    URL https://github.com/lz4/lz4/releases/download/v1.10.0/lz4-1.10.0.tar.gz
    SOURCE_SUBDIR _none)

FetchContent_MakeAvailable(libdeflate nlohmann_json rbscan_lz4)
set_target_properties(libdeflate_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(rbscan_lz4 STATIC "${rbscan_lz4_SOURCE_DIR}/lib/lz4.c")
target_include_directories(rbscan_lz4 PUBLIC "${rbscan_lz4_SOURCE_DIR}/lib")
set_target_properties(rbscan_lz4 PROPERTIES POSITION_INDEPENDENT_CODE ON)
