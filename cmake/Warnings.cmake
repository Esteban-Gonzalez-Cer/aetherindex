# Project-wide warning level. The tree builds warning-free at this level; CI turns the
# warnings into errors with -DAETHER_WARNINGS_AS_ERRORS=ON.
option(AETHER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

add_library(aether_warnings INTERFACE)
if(MSVC)
    target_compile_options(aether_warnings INTERFACE /W4)
    if(AETHER_WARNINGS_AS_ERRORS)
        target_compile_options(aether_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(aether_warnings INTERFACE -Wall -Wextra -Wpedantic)
    if(AETHER_WARNINGS_AS_ERRORS)
        target_compile_options(aether_warnings INTERFACE -Werror)
    endif()
endif()
