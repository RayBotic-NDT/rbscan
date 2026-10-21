# Shared warning flags, applied via the rbscan::warnings interface target.
add_library(rbscan_warnings INTERFACE)
add_library(rbscan::warnings ALIAS rbscan_warnings)

if(MSVC)
    target_compile_options(rbscan_warnings INTERFACE /W4 /permissive- /utf-8)
    if(RBSCAN_WARNINGS_AS_ERRORS)
        target_compile_options(rbscan_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(rbscan_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
    if(RBSCAN_WARNINGS_AS_ERRORS)
        target_compile_options(rbscan_warnings INTERFACE -Werror)
    endif()
endif()
