cmake_minimum_required(VERSION 3.20)
# Optimization regression gate: the default CLI path must stay byte-identical to the
# outputs captured before the optimization phases (timing lines excluded, since they
# vary per run). Regenerate deliberately with -DUPDATE_GOLDEN=ON and review the diff.
# hybrid_q002 was regenerated when convex fusion became the hybrid default; the original
# RRF output is kept, unchanged, as hybrid_q002_rrf (--fusion rrf).
file(MAKE_DIRECTORY "${WORK_DIR}")
execute_process(COMMAND "${GENERATOR_PATH}" "${WORK_DIR}/mvp" RESULT_VARIABLE status
    OUTPUT_QUIET ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Fixture generation failed: ${errors}")
endif()

set(D "${SOURCE_DIR}/datasets")
set(cases
    "brute_force_cosine|search~--algorithm~brute-force~--dataset~${D}/examples/vectors.csv~--query~${D}/examples/query.csv~--k~2"
    "brute_force_squared|search~--algorithm~brute-force~--metric~squared-euclidean~--dataset~${D}/examples/vectors.csv~--query~${D}/examples/query.csv~--k~10"
    "hnsw_examples|search~--algorithm~hnsw~--dataset~${D}/examples/vectors.csv~--query~${D}/examples/query.csv~--k~10"
    "hnsw_mvp_cosine|search~--algorithm~hnsw~--dataset~${WORK_DIR}/mvp/vectors.csv~--query~${WORK_DIR}/mvp/query_3.csv~--k~10"
    "hnsw_mvp_squared_simple|search~--algorithm~hnsw~--metric~squared-euclidean~--neighbor-selection~simple~--m~8~--m0~16~--ef-construction~40~--ef-search~20~--seed~7~--dataset~${WORK_DIR}/mvp/vectors.csv~--query~${WORK_DIR}/mvp/query_8.csv~--k~10"
    "brute_force_mvp|search~--algorithm~brute-force~--dataset~${WORK_DIR}/mvp/vectors.csv~--query~${WORK_DIR}/mvp/query_5.csv~--k~10"
    "lexical_q002|search~--mode~lexical~--dataset~${D}/corpus.jsonl~--query~${D}/queries.jsonl~--query-id~Q002~--k~5"
    "lexical_text|search~--algorithm~bm25~--dataset~${D}/corpus.jsonl~--query-text~HNSW grafo vecinos búsqueda~--k~10~--bm25-k1~1.5~--bm25-b~0.5"
    "vector_jsonl|search~--algorithm~brute-force~--dataset~${D}/hybrid/corpus.jsonl~--query~${D}/hybrid/queries.jsonl~--query-id~Q001~--k~5"
    "hybrid_q002|search~--mode~hybrid~--dataset~${D}/hybrid/corpus.jsonl~--query~${D}/hybrid/queries.jsonl~--query-id~Q002~--k~10"
    "hybrid_q002_rrf|search~--mode~hybrid~--fusion~rrf~--dataset~${D}/hybrid/corpus.jsonl~--query~${D}/hybrid/queries.jsonl~--query-id~Q002~--k~10"
    "hybrid_q017_params|search~--mode~hybrid~--dataset~${D}/hybrid/corpus.jsonl~--query~${D}/hybrid/queries.jsonl~--query-id~Q017~--k~5~--candidate-k~8~--rrf-k~20~--ef-search~16~--metric~squared-euclidean"
)

set(mismatches "")
foreach(entry IN LISTS cases)
    string(FIND "${entry}" "|" bar)
    string(SUBSTRING "${entry}" 0 ${bar} name)
    math(EXPR start "${bar}+1")
    string(SUBSTRING "${entry}" ${start} -1 arguments)
    string(REPLACE "~" ";" arguments "${arguments}")
    execute_process(COMMAND "${CLI_PATH}" ${arguments}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 60)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "${name}: exit ${status}\n${errors}")
    endif()
    string(REPLACE "\r\n" "\n" output "${output}")
    # Drop run-dependent timing values; outputs never print input paths.
    string(REGEX REPLACE "[^\n]*time \\(ms\\):[^\n]*\n" "" output "${output}")
    set(golden "${GOLDEN_DIR}/${name}.txt")
    if(UPDATE_GOLDEN)
        file(WRITE "${golden}" "${output}")
        message(STATUS "Updated ${golden}")
    else()
        if(NOT EXISTS "${golden}")
            message(FATAL_ERROR "Missing golden file ${golden}; run with -DUPDATE_GOLDEN=ON")
        endif()
        file(READ "${golden}" expected)
        if(NOT output STREQUAL expected)
            file(WRITE "${WORK_DIR}/${name}.actual.txt" "${output}")
            list(APPEND mismatches "${name} (actual: ${WORK_DIR}/${name}.actual.txt)")
        endif()
    endif()
endforeach()
if(mismatches)
    string(REPLACE ";" "\n  " mismatches "${mismatches}")
    message(FATAL_ERROR "Default-path output changed:\n  ${mismatches}")
endif()
list(LENGTH cases count)
message(STATUS "Golden CLI outputs: ${count} cases identical")
