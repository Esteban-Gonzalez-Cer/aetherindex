option(AETHER_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(aether_sanitizers INTERFACE)
if(AETHER_ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$" OR MSVC)
        message(FATAL_ERROR "Sanitizers require a supported GCC/Clang compiler and driver.")
    endif()
    target_compile_options(aether_sanitizers INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer
    )
    target_link_options(aether_sanitizers INTERFACE -fsanitize=address,undefined)
endif()

# ThreadSanitizer for the parallel paths (build, batch queries, concurrent retrievers).
# It cannot be combined with AddressSanitizer, so it is a separate build profile.
option(AETHER_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
if(AETHER_ENABLE_TSAN)
    if(AETHER_ENABLE_SANITIZERS)
        message(FATAL_ERROR "AETHER_ENABLE_TSAN cannot be combined with AETHER_ENABLE_SANITIZERS.")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$" OR MSVC)
        message(FATAL_ERROR "ThreadSanitizer requires a supported GCC/Clang compiler and driver.")
    endif()
    target_compile_options(aether_sanitizers INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(aether_sanitizers INTERFACE -fsanitize=thread)
endif()
