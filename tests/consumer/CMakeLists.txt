# Builds against an installed rbscan SDK, the way a user project does:
#   cmake -S tests/consumer -B build-consumer -DCMAKE_PREFIX_PATH=<sdk>
#   cmake --build build-consumer --config Release
#   build-consumer/[Release/]rbscan_consumer <scratch dir>
cmake_minimum_required(VERSION 3.24)
project(rbscan_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(rbscan CONFIG REQUIRED)

add_executable(rbscan_consumer main.cpp)
target_link_libraries(rbscan_consumer PRIVATE rbscan::rbscan)

# rbscan.dll and rbscan_hdf5.dll next to the executable.
if(WIN32)
    add_custom_command(TARGET rbscan_consumer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RBSCAN_RUNTIME_DLLS}
            $<TARGET_FILE_DIR:rbscan_consumer>)
endif()
