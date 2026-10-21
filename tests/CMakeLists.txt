include(FetchContent)

FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(rbscan_tests
    schema_test.cpp
    scan_test.cpp
    segmented_test.cpp)
target_link_libraries(rbscan_tests PRIVATE rbscan::rbscan GTest::gtest_main rbscan::warnings)

include(GoogleTest)
gtest_discover_tests(rbscan_tests DISCOVERY_MODE PRE_TEST)

if(RBSCAN_BUILD_PYTHON)
    add_test(NAME python_tests
        COMMAND ${Python_EXECUTABLE} -m pytest -q "${PROJECT_SOURCE_DIR}/python/tests")
    set_tests_properties(python_tests PROPERTIES
        ENVIRONMENT "PYTHONPATH=${CMAKE_BINARY_DIR}/python/$<CONFIG>")
endif()
