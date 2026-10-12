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
