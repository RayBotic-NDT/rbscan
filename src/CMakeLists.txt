set(_schemas_cpp "${CMAKE_CURRENT_BINARY_DIR}/embedded_schemas.cpp")
rbscan_embed_schemas("${_schemas_cpp}" "${PROJECT_SOURCE_DIR}/schemas")

add_library(rbscan SHARED
    compression.cpp
    convert.cpp
    geometry.cpp
    h5.cpp
    internal.cpp
    raw_file.cpp
    reader.cpp
    schema.cpp
    segmented_writer.cpp
    series_layout.cpp
    tools.cpp
    types.cpp
    vds.cpp
    writer.cpp
    "${_schemas_cpp}")
add_library(rbscan::rbscan ALIAS rbscan)

target_include_directories(rbscan
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(rbscan
    PRIVATE RBSCAN_BUILDING RBSCAN_VERSION="${PROJECT_VERSION}")
# std::thread needs libpthread explicitly on glibc < 2.34 (e.g. manylinux_2_28).
find_package(Threads REQUIRED)
target_link_libraries(rbscan
    PRIVATE
        Threads::Threads
        ${RBSCAN_HDF5_TARGET}
        libdeflate_static
        rbscan_lz4
        nlohmann_json::nlohmann_json
        rbscan::warnings)
set_target_properties(rbscan PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # The HDF5 library lies next to it (SDK lib/, wheel package dir).
    INSTALL_RPATH "$ORIGIN")
# A wheel holds plain files: one librbscan.so instead of versioned file + links.
if(NOT SKBUILD)
    set_target_properties(rbscan PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})
endif()

# hdf5.dll next to rbscan.dll, so tests, examples and the Python module find it.
if(WIN32)
    add_custom_command(TARGET rbscan POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${RBSCAN_HDF5_TARGET}> $<TARGET_FILE_DIR:rbscan>)
endif()

if(NOT RBSCAN_INSTALL)
    return()
endif()

include(GNUInstallDirs)
install(TARGETS rbscan EXPORT rbscanTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT sdk
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT sdk
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT sdk)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT sdk)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/schemas/" DESTINATION ${CMAKE_INSTALL_DATADIR}/rbscan/schemas COMPONENT sdk)
if(WIN32)
    set(_hdf5_destination ${CMAKE_INSTALL_BINDIR})
else()
    set(_hdf5_destination ${CMAKE_INSTALL_LIBDIR})
endif()
install(FILES "${RBSCAN_HDF5_RUNTIME_FILE}" DESTINATION ${_hdf5_destination}
    RENAME "${RBSCAN_HDF5_RUNTIME_NAME}" COMPONENT sdk)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rbscan COMPONENT sdk)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/LICENSES/" DESTINATION ${CMAKE_INSTALL_DATADIR}/rbscan/licenses COMPONENT sdk)
install(EXPORT rbscanTargets NAMESPACE rbscan:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbscan COMPONENT sdk)

include(CMakePackageConfigHelpers)
configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/rbscanConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/rbscanConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbscan)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/rbscanConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/rbscanConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/rbscanConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbscan
    COMPONENT sdk)
