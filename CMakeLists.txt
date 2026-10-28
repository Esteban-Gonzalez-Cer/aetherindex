cmake_minimum_required(VERSION 3.20)

project(AetherIndex LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/Sanitizers.cmake)

add_library(aether_core INTERFACE)
target_include_directories(aether_core INTERFACE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(aether_core INTERFACE cxx_std_20)
target_link_libraries(aether_core INTERFACE aether_sanitizers)

add_library(aether_storage STATIC src/storage/vector_store.cpp)
target_link_libraries(aether_storage PUBLIC aether_core)

add_library(aether_hybrid STATIC src/hybrid/rrf.cpp)
target_link_libraries(aether_hybrid PUBLIC aether_core)

add_library(aether_lexical STATIC
    src/lexical/tokenizer.cpp
    src/lexical/inverted_index.cpp
    src/lexical/bm25.cpp
)
target_link_libraries(aether_lexical PUBLIC aether_core)
if(MSVC)
    target_compile_options(aether_lexical PUBLIC /utf-8)
endif()

option(AETHER_NATIVE_SIMD "Compile distance kernels for the build host (binary may not be portable)" OFF)
add_library(aether_distance STATIC src/distance/scalar_distance.cpp src/distance/distance.cpp)
target_link_libraries(aether_distance PUBLIC aether_core)
if(AETHER_NATIVE_SIMD)
    if(MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|amd64|x86_64")
        target_compile_options(aether_distance PRIVATE /arch:AVX2)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(aether_distance PRIVATE -march=native)
    else()
        message(FATAL_ERROR "AETHER_NATIVE_SIMD is unsupported by this compiler/architecture")
    endif()
endif()

add_library(aether_brute_force STATIC src/ann/brute_force/brute_force_index.cpp)
target_link_libraries(aether_brute_force
    PUBLIC aether_core aether_storage
    PRIVATE aether_distance
)

add_library(aether_hnsw STATIC src/ann/hnsw/hnsw_index.cpp)
target_link_libraries(aether_hnsw
    PUBLIC aether_core aether_storage
    PRIVATE aether_distance
)

add_library(aether_application STATIC
    src/application/query_engine.cpp
    src/application/search_service.cpp
    src/application/csv_loader.cpp
    src/application/json_loader.cpp
)
target_link_libraries(aether_application
    PUBLIC aether_core aether_storage
    PRIVATE aether_brute_force aether_hnsw aether_lexical
)

add_library(aether_cli STATIC apps/cli/cli_parser.cpp apps/cli/cli_runner.cpp)
target_include_directories(aether_cli PUBLIC "${PROJECT_SOURCE_DIR}/apps/cli")
target_link_libraries(aether_cli PUBLIC aether_application)

add_executable(aetherindex apps/cli/main.cpp)
target_link_libraries(aetherindex PRIVATE aether_cli)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

option(AETHER_BUILD_BENCHMARKS "Build optional optimization/performance drivers" OFF)
if(AETHER_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
