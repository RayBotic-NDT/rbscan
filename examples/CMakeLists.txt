add_executable(rbscan_record_ct_scan record_ct_scan.cpp)
target_link_libraries(rbscan_record_ct_scan PRIVATE rbscan::rbscan rbscan::warnings)
