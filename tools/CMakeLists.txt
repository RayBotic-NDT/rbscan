include(GNUInstallDirs)

add_executable(rbscan_cli rbscan_cli.cpp)
set_target_properties(rbscan_cli PROPERTIES OUTPUT_NAME rbscan)
target_link_libraries(rbscan_cli PRIVATE rbscan::rbscan rbscan::warnings)

if(RBSCAN_INSTALL)
    install(TARGETS rbscan_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT sdk)
endif()
