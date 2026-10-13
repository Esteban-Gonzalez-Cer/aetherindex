add_executable(aether_vector_store_tests unit/vector_store_test.cpp)
target_link_libraries(aether_vector_store_tests PRIVATE aether_storage)
add_test(NAME unit.vector_store COMMAND aether_vector_store_tests)

add_executable(aether_vector_store_allocation_tests unit/vector_store_allocation_test.cpp)
target_link_libraries(aether_vector_store_allocation_tests PRIVATE aether_storage)
add_test(NAME unit.vector_store_allocation COMMAND aether_vector_store_allocation_tests)

add_executable(aether_distance_tests unit/distance_test.cpp)
target_link_libraries(aether_distance_tests PRIVATE aether_distance)
add_test(NAME unit.distance COMMAND aether_distance_tests)

add_executable(aether_brute_force_tests unit/brute_force_test.cpp)
target_link_libraries(aether_brute_force_tests PRIVATE aether_brute_force)
add_test(NAME unit.brute_force COMMAND aether_brute_force_tests)

add_executable(aether_storage_distance_tests integration/vector_store_distance_test.cpp)
target_link_libraries(aether_storage_distance_tests PRIVATE aether_storage aether_distance)
add_test(NAME integration.storage_distance COMMAND aether_storage_distance_tests)

add_executable(aether_query_engine_tests integration/dense_retrieval_test.cpp)
target_link_libraries(aether_query_engine_tests PRIVATE aether_application)
add_test(NAME integration.dense_retrieval COMMAND aether_query_engine_tests)

add_test(NAME e2e.cli_demo
    COMMAND "${CMAKE_COMMAND}"
        "-DCLI_PATH=$<TARGET_FILE:aetherindex>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/cli_demo_test.cmake"
)

set_tests_properties(
    unit.vector_store unit.vector_store_allocation unit.distance unit.brute_force
    integration.storage_distance integration.dense_retrieval e2e.cli_demo
    PROPERTIES TIMEOUT 30
)

add_executable(aether_hnsw_tests hnsw/hnsw_tests.cpp)
target_link_libraries(aether_hnsw_tests PRIVATE aether_hnsw aether_brute_force)
target_include_directories(aether_hnsw_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/ann/hnsw")
foreach(T T4.1 T4.2 T4.3 T4.4 T4.5 T4.6)
    add_test(NAME hnsw.${T} COMMAND aether_hnsw_tests ${T})
    set_tests_properties(hnsw.${T} PROPERTIES TIMEOUT 900)
endforeach()