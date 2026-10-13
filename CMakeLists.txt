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

add_library(aether_distance STATIC src/distance/scalar_distance.cpp)
target_link_libraries(aether_distance PUBLIC aether_core)

add_library(aether_brute_force STATIC src/ann/brute_force/brute_force_index.cpp)
target_link_libraries(aether_brute_force
    PUBLIC aether_core aether_storage
    PRIVATE aether_distance
)

add_library(aether_hnsw STATIC src/ann/hnsw/HNSWIndex.cpp)
target_link_libraries(aether_hnsw PUBLIC aether_core aether_storage)

add_library(aether_application STATIC src/application/query_engine.cpp)
target_link_libraries(aether_application
    PUBLIC aether_core aether_storage
    PRIVATE aether_brute_force
)

add_executable(aetherindex apps/cli/main.cpp)
target_link_libraries(aetherindex PRIVATE aether_application)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

