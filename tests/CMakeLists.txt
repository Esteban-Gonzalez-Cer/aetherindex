add_executable(aether_json_value_tests unit/json_value_test.cpp)
target_link_libraries(aether_json_value_tests PRIVATE aether_application)
add_test(NAME unit.json_value COMMAND aether_json_value_tests)
set_tests_properties(unit.json_value PROPERTIES TIMEOUT 30 LABELS "application;jsonl;bm25-cli")

add_executable(aether_lexical_service_tests integration/lexical_service_test.cpp)
target_link_libraries(aether_lexical_service_tests PRIVATE aether_application)
add_test(NAME integration.lexical_service COMMAND aether_lexical_service_tests
    "${CMAKE_CURRENT_BINARY_DIR}/lexical-service/$<CONFIG>")
set_tests_properties(integration.lexical_service PROPERTIES TIMEOUT 60 LABELS "application;lexical;bm25-cli")

add_test(NAME e2e.cli_bm25 COMMAND "${CMAKE_COMMAND}"
    "-DCLI_PATH=$<TARGET_FILE:aetherindex>"
    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/bm25-cli/$<CONFIG>"
    "-DCORPUS_DIR=${PROJECT_SOURCE_DIR}/datasets"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/cli_bm25_test.cmake")
set_tests_properties(e2e.cli_bm25 PROPERTIES TIMEOUT 120 LABELS "application;lexical;bm25-cli")

add_executable(aether_bm25_cli_corpus_tests integration/bm25_cli_corpus_test.cpp)
target_link_libraries(aether_bm25_cli_corpus_tests PRIVATE aether_application)
add_test(NAME integration.bm25_cli_corpus COMMAND aether_bm25_cli_corpus_tests
    "${PROJECT_SOURCE_DIR}/datasets" "${CMAKE_CURRENT_BINARY_DIR}/bm25-cli-corpus/$<CONFIG>")
set_tests_properties(integration.bm25_cli_corpus PROPERTIES TIMEOUT 60 LABELS "application;lexical;bm25-cli")

# CP-04: hybrid HNSW + BM25 + RRF orchestration (DoD level 2 scenarios).
add_executable(aether_hybrid_retrieval_tests integration/hybrid_retrieval_test.cpp)
target_link_libraries(aether_hybrid_retrieval_tests PRIVATE aether_application aether_hybrid)
add_test(NAME integration.hybrid_retrieval COMMAND aether_hybrid_retrieval_tests
    "${CMAKE_CURRENT_BINARY_DIR}/hybrid-retrieval/$<CONFIG>")
set_tests_properties(integration.hybrid_retrieval PROPERTIES TIMEOUT 60 LABELS "hybrid;cp04")

add_test(NAME e2e.cli_hybrid COMMAND "${CMAKE_COMMAND}"
    "-DCLI_PATH=$<TARGET_FILE:aetherindex>"
    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/hybrid-cli/$<CONFIG>"
    "-DDATA_DIR=${PROJECT_SOURCE_DIR}/datasets/hybrid"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/cli_hybrid_test.cmake")
set_tests_properties(e2e.cli_hybrid PROPERTIES TIMEOUT 300 LABELS "hybrid;cp04;kr1_5;sprint2")

# Sprint 2 / OKR 1 formal evaluation: KR1.3 on the labelled CP-01 corpus and KR1.5 on
# its hybrid derivative. Reports (CSV + JSON summaries) are written next to the build.
add_executable(aether_sprint2_okr1_evaluate integration/sprint2_okr1_test.cpp)
target_link_libraries(aether_sprint2_okr1_evaluate PRIVATE aether_application)
add_test(NAME acceptance.kr1_3_bm25 COMMAND aether_sprint2_okr1_evaluate kr1_3
    "${PROJECT_SOURCE_DIR}/datasets/corpus.jsonl" "${PROJECT_SOURCE_DIR}/datasets/queries.jsonl"
    "${CMAKE_CURRENT_BINARY_DIR}/sprint2-okr1/$<CONFIG>")
set_tests_properties(acceptance.kr1_3_bm25 PROPERTIES TIMEOUT 120 LABELS "lexical;sprint2;kr1_3")
add_test(NAME acceptance.kr1_5_hybrid COMMAND aether_sprint2_okr1_evaluate kr1_5
    "${PROJECT_SOURCE_DIR}/datasets/hybrid/corpus.jsonl"
    "${PROJECT_SOURCE_DIR}/datasets/hybrid/queries.jsonl"
    "${CMAKE_CURRENT_BINARY_DIR}/sprint2-okr1/$<CONFIG>")
set_tests_properties(acceptance.kr1_5_hybrid PROPERTIES TIMEOUT 300 LABELS "hybrid;sprint2;kr1_5")

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

# Opt-in weighted RRF and convex fusion (market comparison).
add_executable(aether_fusion_weights_tests unit/fusion_weights_test.cpp)
target_link_libraries(aether_fusion_weights_tests PRIVATE aether_hybrid)
add_test(NAME unit.fusion_weights COMMAND aether_fusion_weights_tests)
set_tests_properties(unit.fusion_weights PROPERTIES TIMEOUT 60 LABELS "hybrid;fusion;market")

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

# Optional TSV evaluation corpus; independent of the JSONL CLI corpus. This
# records preliminary HitRate without asserting the CP-05 goal.
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

# Optimization regression gate: default-path CLI outputs must remain byte-identical
# to tests/e2e/golden (captured before the optimization phases; timing lines ignored).
add_test(NAME e2e.cli_golden COMMAND "${CMAKE_COMMAND}"
    "-DCLI_PATH=$<TARGET_FILE:aetherindex>"
    "-DGENERATOR_PATH=$<TARGET_FILE:aether_cli_fixture_generator>"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DGOLDEN_DIR=${CMAKE_CURRENT_SOURCE_DIR}/e2e/golden"
    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/golden/$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/cli_golden_test.cmake")
set_tests_properties(e2e.cli_golden PROPERTIES TIMEOUT 120 LABELS "regression;golden")

# Optimization phase 1: binary .avec embeddings.
add_executable(aether_vector_file_tests unit/vector_file_test.cpp)
target_link_libraries(aether_vector_file_tests PRIVATE aether_storage)
add_test(NAME unit.vector_file COMMAND aether_vector_file_tests
    "${CMAKE_CURRENT_BINARY_DIR}/vector-file/$<CONFIG>")
set_tests_properties(unit.vector_file PROPERTIES TIMEOUT 60 LABELS "storage;optimization;avec")
add_test(NAME e2e.cli_avec COMMAND "${CMAKE_COMMAND}"
    "-DCLI_PATH=$<TARGET_FILE:aetherindex>"
    "-DGENERATOR_PATH=$<TARGET_FILE:aether_cli_fixture_generator>"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/avec-cli/$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/cli_avec_test.cmake")
set_tests_properties(e2e.cli_avec PROPERTIES TIMEOUT 300 LABELS "optimization;avec")

# Optimization phase 2: parallel construction, batch queries, concurrent retrievers.
add_executable(aether_hnsw_parallel_build_tests unit/hnsw_parallel_build_test.cpp)
target_link_libraries(aether_hnsw_parallel_build_tests PRIVATE aether_hnsw aether_brute_force)
add_test(NAME unit.hnsw_parallel_build COMMAND aether_hnsw_parallel_build_tests)
set_tests_properties(unit.hnsw_parallel_build PROPERTIES TIMEOUT 300 LABELS "hnsw;optimization;parallel")
add_test(NAME e2e.cli_batch COMMAND "${CMAKE_COMMAND}"
    "-DCLI_PATH=$<TARGET_FILE:aetherindex>"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/batch-cli/$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/cli_batch_test.cmake")
set_tests_properties(e2e.cli_batch PROPERTIES TIMEOUT 600 LABELS "optimization;parallel")

# Optimization phases 3-4: SQ8/PQ codes and quantized HNSW traversal.
add_executable(aether_quantization_tests integration/quantization_test.cpp)
target_link_libraries(aether_quantization_tests PRIVATE aether_quantization aether_hnsw aether_brute_force)
add_test(NAME integration.quantization COMMAND aether_quantization_tests)
set_tests_properties(integration.quantization PROPERTIES TIMEOUT 300 LABELS "quantization;optimization")
add_test(NAME e2e.cli_quantization COMMAND "${CMAKE_COMMAND}"
    "-DCLI_PATH=$<TARGET_FILE:aetherindex>"
    "-DGENERATOR_PATH=$<TARGET_FILE:aether_cli_fixture_generator>"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DMETHODS=sq8;pq"
    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/quantization-cli/$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/cli_quantization_test.cmake")
set_tests_properties(e2e.cli_quantization PROPERTIES TIMEOUT 300 LABELS "quantization;optimization")

# Optimization phase 5: compressed BM25 strategies (TAAT, WAND, Block-Max WAND).
add_executable(aether_bm25_strategies_tests unit/bm25_strategies_test.cpp)
target_link_libraries(aether_bm25_strategies_tests PRIVATE aether_lexical)
add_test(NAME unit.bm25_strategies COMMAND aether_bm25_strategies_tests)
set_tests_properties(unit.bm25_strategies PROPERTIES TIMEOUT 600 LABELS "lexical;optimization;bm25-scale")

# Market comparison: opt-in english-v1 analyzer (Porter, possessives, stopwords).
add_executable(aether_english_analyzer_tests unit/english_analyzer_test.cpp)
target_link_libraries(aether_english_analyzer_tests PRIVATE aether_lexical)
add_test(NAME unit.english_analyzer COMMAND aether_english_analyzer_tests)
set_tests_properties(unit.english_analyzer PROPERTIES TIMEOUT 120 LABELS "lexical;market")
add_test(NAME e2e.cli_analyzer COMMAND "${CMAKE_COMMAND}"
    "-DCLI_PATH=$<TARGET_FILE:aetherindex>"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/cli_analyzer_test.cmake")
set_tests_properties(e2e.cli_analyzer PROPERTIES TIMEOUT 120 LABELS "lexical;market")
add_test(NAME e2e.cli_fusion COMMAND "${CMAKE_COMMAND}"
    "-DCLI_PATH=$<TARGET_FILE:aetherindex>"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/cli_fusion_test.cmake")
set_tests_properties(e2e.cli_fusion PROPERTIES TIMEOUT 120 LABELS "hybrid;fusion;market")

# Optimization phase 6: metadata filtering (dense exact/graph, BM25 strategies, hybrid).
add_executable(aether_filter_tests integration/filter_test.cpp)
target_link_libraries(aether_filter_tests PRIVATE aether_application)
add_test(NAME integration.filter COMMAND aether_filter_tests
    "${CMAKE_CURRENT_BINARY_DIR}/filter/$<CONFIG>")
set_tests_properties(integration.filter PROPERTIES TIMEOUT 300 LABELS "filter;optimization")
add_test(NAME e2e.cli_filter COMMAND "${CMAKE_COMMAND}"
    "-DCLI_PATH=$<TARGET_FILE:aetherindex>"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/cli_filter_test.cmake")
set_tests_properties(e2e.cli_filter PROPERTIES TIMEOUT 120 LABELS "filter;optimization")

add_executable(aether_snapshot_tests unit/snapshot_test.cpp)
target_link_libraries(aether_snapshot_tests PRIVATE aether_persistence)
add_test(NAME unit.snapshot COMMAND aether_snapshot_tests
    "${CMAKE_CURRENT_BINARY_DIR}/snapshot/$<CONFIG>")
set_tests_properties(unit.snapshot PROPERTIES TIMEOUT 120 LABELS "persistence;snapshot")

# Saved indexes: index build + search --index reproduce every golden output.
add_test(NAME e2e.cli_index COMMAND "${CMAKE_COMMAND}"
    "-DCLI_PATH=$<TARGET_FILE:aetherindex>"
    "-DGENERATOR_PATH=$<TARGET_FILE:aether_cli_fixture_generator>"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DGOLDEN_DIR=${CMAKE_CURRENT_SOURCE_DIR}/e2e/golden"
    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/index-cli/$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/cli_index_test.cmake")
set_tests_properties(e2e.cli_index PROPERTIES TIMEOUT 300 LABELS "persistence;e2e")

add_executable(aether_vector_delete_tests unit/vector_delete_test.cpp)
target_link_libraries(aether_vector_delete_tests PRIVATE aether_persistence aether_brute_force)
add_test(NAME unit.vector_delete COMMAND aether_vector_delete_tests)
set_tests_properties(unit.vector_delete PROPERTIES TIMEOUT 120 LABELS "deletes;storage;hnsw")

add_executable(aether_segmented_bm25_tests unit/segmented_bm25_test.cpp)
target_link_libraries(aether_segmented_bm25_tests PRIVATE aether_lexical)
add_test(NAME unit.segmented_bm25 COMMAND aether_segmented_bm25_tests)
set_tests_properties(unit.segmented_bm25 PROPERTIES TIMEOUT 300 LABELS "lexical;deletes;bm25")

add_executable(aether_collection_mutation_tests integration/collection_mutation_test.cpp)
target_link_libraries(aether_collection_mutation_tests PRIVATE aether_application)
add_test(NAME integration.collection_mutation COMMAND aether_collection_mutation_tests
    "${CMAKE_CURRENT_BINARY_DIR}/collection-mutation/$<CONFIG>")
set_tests_properties(integration.collection_mutation PROPERTIES TIMEOUT 300
    LABELS "deletes;persistence;application")

add_test(NAME e2e.cli_index_mutation COMMAND "${CMAKE_COMMAND}"
    "-DCLI_PATH=$<TARGET_FILE:aetherindex>"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/index-mutation-cli/$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/cli_index_mutation_test.cmake")
set_tests_properties(e2e.cli_index_mutation PROPERTIES TIMEOUT 300 LABELS "deletes;persistence;e2e")

add_executable(aether_concurrent_collection_tests integration/concurrent_collection_test.cpp)
target_link_libraries(aether_concurrent_collection_tests PRIVATE aether_application)
add_test(NAME integration.concurrent_collection COMMAND aether_concurrent_collection_tests
    "${CMAKE_CURRENT_BINARY_DIR}/concurrent-collection/$<CONFIG>")
set_tests_properties(integration.concurrent_collection PROPERTIES TIMEOUT 600
    LABELS "concurrency;deletes;application")

# Crash recovery of the write-ahead log and checkpoints (fault injection).
add_test(NAME e2e.cli_crash COMMAND "${CMAKE_COMMAND}"
    "-DCLI_PATH=$<TARGET_FILE:aetherindex>"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/crash-cli/$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/e2e/cli_crash_test.cmake")
set_tests_properties(e2e.cli_crash PROPERTIES TIMEOUT 300 LABELS "persistence;crash;e2e")

add_executable(aether_write_ahead_log_tests unit/write_ahead_log_test.cpp)
target_link_libraries(aether_write_ahead_log_tests PRIVATE aether_persistence)
add_test(NAME unit.write_ahead_log COMMAND aether_write_ahead_log_tests
    "${CMAKE_CURRENT_BINARY_DIR}/write-ahead-log/$<CONFIG>")
set_tests_properties(unit.write_ahead_log PROPERTIES TIMEOUT 60 LABELS "persistence;wal")
