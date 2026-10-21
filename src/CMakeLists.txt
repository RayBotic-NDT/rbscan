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
target_link_libraries(rbscan
    PRIVATE
        ${RBSCAN_HDF5_TARGET}
        libdeflate_static
        rbscan_lz4
        nlohmann_json::nlohmann_json
        rbscan::warnings)
set_target_properties(rbscan PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

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
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY "${PROJECT_SOURCE_DIR}/schemas/" DESTINATION ${CMAKE_INSTALL_DATADIR}/rbscan/schemas)
install(FILES $<TARGET_FILE:${RBSCAN_HDF5_TARGET}> DESTINATION ${CMAKE_INSTALL_BINDIR})
install(EXPORT rbscanTargets NAMESPACE rbscan:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbscan)

include(CMakePackageConfigHelpers)
configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/rbscanConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/rbscanConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbscan)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/rbscanConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/rbscanConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/rbscanConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbscan)
