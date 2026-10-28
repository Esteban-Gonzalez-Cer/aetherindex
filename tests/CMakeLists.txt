add_executable(aether_rrf_tests unit/rrf_test.cpp)
target_link_libraries(aether_rrf_tests PRIVATE aether_hybrid)
foreach(number RANGE 1 22)
    if(number LESS 10)
        set(case_id "RRF-0${number}")
    else()
        set(case_id "RRF-${number}")
    endif()
    add_test(NAME "unit.rrf.${case_id}"
        COMMAND aether_rrf_tests "${case_id}"
            "${CMAKE_CURRENT_BINARY_DIR}/rrf-evidence/$<CONFIG>/${case_id}.json")
    set_tests_properties("unit.rrf.${case_id}" PROPERTIES TIMEOUT 30 LABELS "hybrid;rrf;cp03;kr1_4")
endforeach()
add_test(NAME unit.rrf.properties COMMAND aether_rrf_tests properties
    "${CMAKE_CURRENT_BINARY_DIR}/rrf-evidence/$<CONFIG>/properties.json")
set_tests_properties(unit.rrf.properties PROPERTIES TIMEOUT 60 LABELS "hybrid;rrf;cp03")

add_executable(aether_rrf_contract_tests integration/rrf_contract_test.cpp)
target_link_libraries(aether_rrf_contract_tests PRIVATE aether_hybrid)
add_test(NAME integration.rrf_contract COMMAND aether_rrf_contract_tests)
set_tests_properties(integration.rrf_contract PROPERTIES TIMEOUT 30 LABELS "hybrid;rrf;cp03;contract")

add_executable(aether_jsonl_service_compatibility_tests integration/jsonl_service_compatibility_test.cpp)
target_link_libraries(aether_jsonl_service_compatibility_tests PRIVATE aether_application)
add_test(NAME integration.jsonl_service_compatibility
    COMMAND aether_jsonl_service_compatibility_tests
        "${CMAKE_CURRENT_BINARY_DIR}/jsonl-compatibility/$<CONFIG>")
set_tests_properties(integration.jsonl_service_compatibility PROPERTIES
    TIMEOUT 60 LABELS "application;compatibility")

foreach(component tokenizer inverted_index bm25_statistics bm25_math bm25_search bm25_allocation)
    add_executable(aether_${component}_tests unit/${component}_test.cpp)
    target_link_libraries(aether_${component}_tests PRIVATE aether_lexical)
    add_test(NAME unit.${component} COMMAND aether_${component}_tests)
    set_tests_properties(unit.${component} PROPERTIES TIMEOUT 60 LABELS "lexical;cp02")
endforeach()

add_executable(aether_bm25_contract_tests integration/bm25_contract_test.cpp)
target_link_libraries(aether_bm25_contract_tests PRIVATE aether_lexical)
add_test(NAME integration.bm25_contract COMMAND aether_bm25_contract_tests)
set_tests_properties(integration.bm25_contract PROPERTIES TIMEOUT 60 LABELS "lexical;cp02;contract")

add_executable(aether_bm25_evaluate integration/bm25_corpus_test.cpp)
target_link_libraries(aether_bm25_evaluate PRIVATE aether_lexical)
add_test(NAME integration.bm25_corpus_regression
    COMMAND "${CMAKE_COMMAND}"
        "-DEVALUATOR_PATH=$<TARGET_FILE:aether_bm25_evaluate>"
        "-DDATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bm25"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/bm25-regression/$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/bm25_corpus_test.cmake"
)
set_tests_properties(integration.bm25_corpus_regression PROPERTIES TIMEOUT 120 LABELS "lexical;cp02;regression")

# CP-01 is not present yet. This opt-in consumes its agreed TSV adapter when
# available; it records preliminary HitRate without asserting the CP-05 goal.
set(AETHER_BM25_CORPUS_DIR "" CACHE PATH "Optional labelled BM25 corpus (documents.tsv, queries.tsv)")
if(AETHER_BM25_CORPUS_DIR)
    foreach(name documents.tsv queries.tsv)
        if(NOT EXISTS "${AETHER_BM25_CORPUS_DIR}/${name}")
            message(FATAL_ERROR "Missing BM25 corpus file: ${AETHER_BM25_CORPUS_DIR}/${name}")
        endif()
    endforeach()
    add_test(NAME integration.bm25_corpus_external
        COMMAND aether_bm25_evaluate "${AETHER_BM25_CORPUS_DIR}/documents.tsv"
            "${AETHER_BM25_CORPUS_DIR}/queries.tsv"
            "${CMAKE_CURRENT_BINARY_DIR}/bm25-external/$<CONFIG>")
    set_tests_properties(integration.bm25_corpus_external PROPERTIES TIMEOUT 180 LABELS "lexical;cp02;external-data")
endif()

add_executable(aether_vector_store_tests unit/vector_store_test.cpp)
target_link_libraries(aether_vector_store_tests PRIVATE aether_storage)
add_test(NAME unit.vector_store COMMAND aether_vector_store_tests)

add_executable(aether_vector_store_allocation_tests unit/vector_store_allocation_test.cpp)
target_link_libraries(aether_vector_store_allocation_tests PRIVATE aether_hnsw)
add_test(NAME unit.vector_store_allocation COMMAND aether_vector_store_allocation_tests)

add_executable(aether_distance_tests unit/distance_test.cpp)
target_link_libraries(aether_distance_tests PRIVATE aether_distance)
add_test(NAME unit.distance COMMAND aether_distance_tests)

add_executable(aether_brute_force_tests unit/brute_force_test.cpp)
target_link_libraries(aether_brute_force_tests PRIVATE aether_brute_force)
add_test(NAME unit.brute_force COMMAND aether_brute_force_tests)

add_executable(aether_optimization_contract_tests unit/optimization_contracts_test.cpp)
target_link_libraries(aether_optimization_contract_tests PRIVATE aether_application)
add_test(NAME unit.optimization_contracts COMMAND aether_optimization_contract_tests)
set_tests_properties(unit.optimization_contracts PROPERTIES TIMEOUT 120)

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

# Sprint 1 acceptance uses one shared dataset and a separate, frozen query set.
add_executable(aether_sprint1_okr1_tests integration/sprint1_okr1_test.cpp)
target_link_libraries(aether_sprint1_okr1_tests PRIVATE aether_application)
add_test(NAME integration.sprint1_okr1
    COMMAND aether_sprint1_okr1_tests "${CMAKE_CURRENT_BINARY_DIR}/okr1-$<CONFIG>"
)
set_tests_properties(integration.sprint1_okr1 PROPERTIES
    TIMEOUT 300 LABELS "sprint1;okr1" FIXTURES_SETUP sprint1_okr1_data
)
add_test(NAME e2e.sprint1_okr1
    COMMAND "${CMAKE_COMMAND}"
        "-DCLI_PATH=$<TARGET_FILE:aetherindex>"
        "-DEVIDENCE_DIR=${CMAKE_CURRENT_BINARY_DIR}/okr1-$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/sprint1_okr1_test.cmake"
)
set_tests_properties(e2e.sprint1_okr1 PROPERTIES
    TIMEOUT 180 LABELS "sprint1;okr1" FIXTURES_REQUIRED sprint1_okr1_data
)

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
