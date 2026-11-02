find_package(Python 3.11 COMPONENTS Interpreter Development.Module REQUIRED)

# pybind11 from the build environment (pip / scikit-build-core), else fetched.
if(NOT pybind11_DIR)
    execute_process(COMMAND "${Python_EXECUTABLE}" -m pybind11 --cmakedir
        OUTPUT_VARIABLE _pybind11_dir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_pybind11_dir)
        set(pybind11_DIR "${_pybind11_dir}")
    endif()
endif()
find_package(pybind11 3.0 CONFIG QUIET)
if(NOT pybind11_FOUND)
    FetchContent_Declare(pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG v3.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(pybind11)
endif()

pybind11_add_module(_rbscan MODULE src/bindings.cpp)
target_link_libraries(_rbscan PRIVATE rbscan::rbscan rbscan::warnings)

# Importable package in the build tree: build/python/<config>/rbscan.
set(_pkg "${CMAKE_BINARY_DIR}/python/$<CONFIG>/rbscan")
set_target_properties(_rbscan PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${_pkg}"
    RUNTIME_OUTPUT_DIRECTORY "${_pkg}")
file(GLOB _py_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/rbscan/*.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/rbscan/py.typed")
# A target of its own, so a rebuilt rbscan.dll is copied even when the module
# itself did not relink.
add_custom_target(rbscan_python_package ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_pkg}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_py_sources} "${_pkg}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:rbscan> "${_pkg}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${RBSCAN_HDF5_TARGET}> "${_pkg}"
    DEPENDS rbscan _rbscan
    VERBATIM)
set(RBSCAN_PYTHON_PACKAGE_DIR "${CMAKE_BINARY_DIR}/python/$<CONFIG>" PARENT_SCOPE)

# Wheel (scikit-build-core installs the "python" component only).
set_target_properties(_rbscan PROPERTIES INSTALL_RPATH "$ORIGIN")
install(TARGETS _rbscan rbscan
    LIBRARY DESTINATION rbscan COMPONENT python
    RUNTIME DESTINATION rbscan COMPONENT python)
install(FILES "${RBSCAN_HDF5_RUNTIME_FILE}" DESTINATION rbscan
    RENAME "${RBSCAN_HDF5_RUNTIME_NAME}" COMPONENT python)
