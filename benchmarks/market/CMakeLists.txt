# Market comparison harnesses (see README.md). Release builds only for measurements.
include(FetchContent)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)  # Header-only checkout; no add_subdirectory.
endif()
FetchContent_Declare(hnswlib
    GIT_REPOSITORY https://github.com/nmslib/hnswlib.git
    GIT_TAG v0.8.0
    GIT_SHALLOW TRUE)
FetchContent_GetProperties(hnswlib)
if(NOT hnswlib_POPULATED)
    FetchContent_Populate(hnswlib)
endif()

find_package(faiss CONFIG REQUIRED HINTS /opt/homebrew/opt/faiss/share/faiss)
find_path(OPENMP_INCLUDE omp.h HINTS /opt/homebrew/opt/libomp/include REQUIRED)
find_library(OPENMP_LIBRARY omp HINTS /opt/homebrew/opt/libomp/lib REQUIRED)

# ANN: AetherIndex, hnswlib and FAISS in one process model, same timer and threads.
add_executable(aether_ann_compare ann_compare.cpp)
target_include_directories(aether_ann_compare SYSTEM PRIVATE "${hnswlib_SOURCE_DIR}" "${OPENMP_INCLUDE}")
target_link_libraries(aether_ann_compare PRIVATE aether_hnsw aether_brute_force aether_quantization
    faiss "${OPENMP_LIBRARY}")
target_compile_definitions(aether_ann_compare PRIVATE AETHER_HNSWLIB_VARIANT="default")

# hnswlib 0.8.0 has no ARM SIMD kernels; its scalar loops vectorize only when float
# reassociation is allowed. This binary runs only the hnswlib system with -ffast-math,
# as its best case on this machine.
add_executable(aether_ann_compare_fastmath ann_compare.cpp)
target_include_directories(aether_ann_compare_fastmath SYSTEM PRIVATE "${hnswlib_SOURCE_DIR}" "${OPENMP_INCLUDE}")
target_link_libraries(aether_ann_compare_fastmath PRIVATE aether_hnsw aether_brute_force
    aether_quantization faiss "${OPENMP_LIBRARY}")
target_compile_definitions(aether_ann_compare_fastmath PRIVATE AETHER_HNSWLIB_VARIANT="fastmath")
target_compile_options(aether_ann_compare_fastmath PRIVATE -ffast-math)

# BEIR: AetherIndex BM25 / dense / hybrid runs, latency, and analyzer token dumps.
add_executable(aether_ir_compare ir_compare.cpp)
target_include_directories(aether_ir_compare PRIVATE "${PROJECT_SOURCE_DIR}/src/application")
target_link_libraries(aether_ir_compare PRIVATE aether_application aether_hnsw aether_brute_force
    aether_lexical aether_hybrid aether_quantization aether_persistence)
