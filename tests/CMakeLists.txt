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

add_executable(aether_cli_parser_tests unit/cli_parser_test.cpp)
target_link_libraries(aether_cli_parser_tests PRIVATE aether_cli)
add_test(NAME unit.cli_parser COMMAND aether_cli_parser_tests)

add_executable(aether_search_service_tests integration/search_service_test.cpp)
target_link_libraries(aether_search_service_tests PRIVATE aether_application)
add_test(NAME integration.search_service
    COMMAND aether_search_service_tests "${CMAKE_CURRENT_BINARY_DIR}/service-fixtures/$<CONFIG>"
)

add_executable(aether_cli_fixture_generator fixtures/generate_cli_dataset.cpp)
target_link_libraries(aether_cli_fixture_generator PRIVATE aether_core)

add_test(NAME e2e.cli_search
    COMMAND "${CMAKE_COMMAND}"
        "-DCLI_PATH=$<TARGET_FILE:aetherindex>"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-fixtures/$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/cli_search_test.cmake"
)

add_test(NAME e2e.cli_mvp
    COMMAND "${CMAKE_COMMAND}"
        "-DCLI_PATH=$<TARGET_FILE:aetherindex>"
        "-DGENERATOR_PATH=$<TARGET_FILE:aether_cli_fixture_generator>"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/mvp-fixtures/$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/cli_mvp_test.cmake"
)

set_tests_properties(
    unit.vector_store unit.vector_store_allocation unit.distance unit.brute_force
    integration.storage_distance integration.dense_retrieval unit.cli_parser integration.search_service
    PROPERTIES TIMEOUT 30
)
set_tests_properties(e2e.cli_search e2e.cli_mvp PROPERTIES TIMEOUT 60)

foreach(component node insert search graph)
    add_executable(aether_hnsw_${component}_tests unit/hnsw_${component}_test.cpp)
    target_link_libraries(aether_hnsw_${component}_tests PRIVATE aether_hnsw aether_distance)
    add_test(NAME unit.hnsw_${component} COMMAND aether_hnsw_${component}_tests)
    set_tests_properties(unit.hnsw_${component} PROPERTIES TIMEOUT 120)
endforeach()

add_executable(aether_hnsw_recall_tests integration/hnsw_recall_test.cpp)
target_link_libraries(aether_hnsw_recall_tests PRIVATE aether_hnsw aether_brute_force aether_distance)
add_test(NAME integration.hnsw_recall
    COMMAND aether_hnsw_recall_tests "${CMAKE_CURRENT_BINARY_DIR}/hnsw-recall-$<CONFIG>.csv"
)
set_tests_properties(integration.hnsw_recall PROPERTIES TIMEOUT 300)

add_test(NAME e2e.cli_hnsw
    COMMAND "${CMAKE_COMMAND}"
        "-DCLI_PATH=$<TARGET_FILE:aetherindex>"
        "-DGENERATOR_PATH=$<TARGET_FILE:aether_cli_fixture_generator>"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/hnsw-cli-fixtures/$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/cli_hnsw_test.cmake"
)
set_tests_properties(e2e.cli_hnsw PROPERTIES TIMEOUT 300)

# Explicit opt-in: the normal suite stays offline and dependency-free.
set(AETHER_REAL_DATA_DIR "" CACHE PATH "Prepared DBpedia directory for the optional real-data CLI test")
if(AETHER_REAL_DATA_DIR)
    if(NOT EXISTS "${AETHER_REAL_DATA_DIR}/manifest.json")
        message(FATAL_ERROR "Prepare the real dataset first with scripts/prepare_real_dataset.py")
    endif()
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_test(NAME e2e.cli_real_dataset
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/validate_real_dataset.py"
            --cli "$<TARGET_FILE:aetherindex>" --data-dir "${AETHER_REAL_DATA_DIR}" --k 100
            --report "${CMAKE_CURRENT_BINARY_DIR}/real-data-$<CONFIG>.json"
    )
    set_tests_properties(e2e.cli_real_dataset PROPERTIES TIMEOUT 600 LABELS "real-data")

    add_executable(aether_hnsw_real_dataset_tests integration/hnsw_real_dataset_test.cpp)
    target_link_libraries(aether_hnsw_real_dataset_tests
        PRIVATE aether_hnsw aether_brute_force aether_distance
    )
    add_test(NAME integration.hnsw_real_dataset
        COMMAND aether_hnsw_real_dataset_tests "${AETHER_REAL_DATA_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/hnsw-real-data-$<CONFIG>.csv"
    )
    set_tests_properties(integration.hnsw_real_dataset PROPERTIES TIMEOUT 1800 LABELS "real-data")
endif()
