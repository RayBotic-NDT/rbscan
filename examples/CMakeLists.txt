add_executable(rbscan_record_ct_scan record_ct_scan.cpp)
target_link_libraries(rbscan_record_ct_scan PRIVATE rbscan::rbscan rbscan::warnings)

add_executable(rbscan_read_benchmark read_benchmark.cpp)
find_package(Threads REQUIRED)
target_link_libraries(rbscan_read_benchmark PRIVATE rbscan::rbscan rbscan::warnings Threads::Threads)

# The typed API (schemas.hpp, schema_helpers.hpp), one example per kind of node.
foreach(example typed_ct_scan simulation_scene plan_and_annotate)
    add_executable(rbscan_${example} ${example}.cpp)
    target_link_libraries(rbscan_${example} PRIVATE rbscan::rbscan rbscan::warnings)
endforeach()
