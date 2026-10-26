add_executable(aether_optimization_benchmark optimization_benchmark.cpp)
target_link_libraries(aether_optimization_benchmark PRIVATE aether_application)

add_executable(aether_dbpedia_benchmark dbpedia_benchmark.cpp)
target_link_libraries(aether_dbpedia_benchmark PRIVATE aether_application)

# Performance thresholds are opt-in, machine-dependent acceptance checks;
# ordinary dependency-free correctness CTest does not fail on slower hosts.
option(AETHER_REGISTER_PERFORMANCE_TESTS "Register opt-in benchmark acceptance checks in CTest" OFF)
if(AETHER_REGISTER_PERFORMANCE_TESTS)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    foreach(workload topk memory clustered)
        add_test(NAME performance.${workload}
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/check_optimization_benchmark.py"
                --benchmark "$<TARGET_FILE:aether_optimization_benchmark>" --workload ${workload}
                --output "${CMAKE_CURRENT_BINARY_DIR}/evidence/${workload}")
        set_tests_properties(performance.${workload} PROPERTIES TIMEOUT 1800 LABELS "performance" RUN_SERIAL TRUE)
    endforeach()
    if(AETHER_REAL_DATA_DIR)
        add_test(NAME performance.dbpedia
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/check_optimization_benchmark.py"
                --benchmark "$<TARGET_FILE:aether_dbpedia_benchmark>" --workload dbpedia
                --data-dir "${AETHER_REAL_DATA_DIR}"
                --output "${CMAKE_CURRENT_BINARY_DIR}/evidence/dbpedia")
        set_tests_properties(performance.dbpedia PROPERTIES TIMEOUT 1800 LABELS "performance;real-data" RUN_SERIAL TRUE)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND
       (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" OR AETHER_NATIVE_SIMD))
        set(assembly_options)
        if(AETHER_NATIVE_SIMD)
            list(APPEND assembly_options --native)
        endif()
        add_test(NAME performance.assembly
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/verify_distance_assembly.py"
                --compiler "${CMAKE_CXX_COMPILER}" --source-root "${PROJECT_SOURCE_DIR}"
                --output "${CMAKE_CURRENT_BINARY_DIR}/evidence/assembly" ${assembly_options})
        set_tests_properties(performance.assembly PROPERTIES TIMEOUT 60 LABELS "assembly")
    endif()
endif()
