cmake_minimum_required(VERSION 3.20)
# Saved indexes: every golden CLI case is replayed as `index build` followed by
# `search --index`, and must print exactly the golden output (timing lines excluded,
# since they vary per run, and the load-time scope line, which only a saved index prints).
# Then the index commands' error handling: misplaced options, a non-empty output
# directory, damaged files, and verify/info.
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
execute_process(COMMAND "${GENERATOR_PATH}" "${WORK_DIR}/mvp" RESULT_VARIABLE status
    OUTPUT_QUIET ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Fixture generation failed: ${errors}")
endif()

set(D "${SOURCE_DIR}/datasets")
set(M "${WORK_DIR}/mvp")
# name | build options | query options   ("~" separates arguments)
set(cases
    "brute_force_cosine|--algorithm~brute-force~--dataset~${D}/examples/vectors.csv|--query~${D}/examples/query.csv~--k~2"
    "brute_force_squared|--algorithm~brute-force~--metric~squared-euclidean~--dataset~${D}/examples/vectors.csv|--query~${D}/examples/query.csv~--k~10"
    "hnsw_examples|--algorithm~hnsw~--dataset~${D}/examples/vectors.csv|--query~${D}/examples/query.csv~--k~10"
    "hnsw_mvp_cosine|--algorithm~hnsw~--dataset~${M}/vectors.csv|--query~${M}/query_3.csv~--k~10"
    "hnsw_mvp_squared_simple|--algorithm~hnsw~--metric~squared-euclidean~--neighbor-selection~simple~--m~8~--m0~16~--ef-construction~40~--seed~7~--dataset~${M}/vectors.csv|--ef-search~20~--query~${M}/query_8.csv~--k~10"
    "brute_force_mvp|--algorithm~brute-force~--dataset~${M}/vectors.csv|--query~${M}/query_5.csv~--k~10"
    "lexical_q002|--mode~lexical~--dataset~${D}/corpus.jsonl|--query~${D}/queries.jsonl~--query-id~Q002~--k~5"
    "lexical_text|--algorithm~bm25~--dataset~${D}/corpus.jsonl~--bm25-k1~1.5~--bm25-b~0.5|--query-text~HNSW grafo vecinos búsqueda~--k~10"
    "vector_jsonl|--algorithm~brute-force~--dataset~${D}/hybrid/corpus.jsonl|--query~${D}/hybrid/queries.jsonl~--query-id~Q001~--k~5"
    "hybrid_q002|--mode~hybrid~--dataset~${D}/hybrid/corpus.jsonl|--query~${D}/hybrid/queries.jsonl~--query-id~Q002~--k~10"
    "hybrid_q002_rrf|--mode~hybrid~--dataset~${D}/hybrid/corpus.jsonl|--fusion~rrf~--query~${D}/hybrid/queries.jsonl~--query-id~Q002~--k~10"
    "hybrid_q017_params|--mode~hybrid~--metric~squared-euclidean~--dataset~${D}/hybrid/corpus.jsonl|--query~${D}/hybrid/queries.jsonl~--query-id~Q017~--k~5~--candidate-k~8~--rrf-k~20~--ef-search~16"
)

function(run_cli expected_status)
    execute_process(COMMAND "${CLI_PATH}" ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 120)
    if(NOT status EQUAL expected_status)
        string(REPLACE ";" " " shown "${ARGN}")
        message(FATAL_ERROR "Expected exit ${expected_status}, got ${status}: ${shown}\n${output}\n${errors}")
    endif()
    set(cli_output "${output}" PARENT_SCOPE)
    set(cli_errors "${errors}" PARENT_SCOPE)
endfunction()

function(require_fragment text fragment context)
    string(FIND "${text}" "${fragment}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "${context}: missing '${fragment}' in:\n${text}")
    endif()
endfunction()

set(mismatches "")
foreach(entry IN LISTS cases)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 name)
    list(GET parts 1 build)
    list(GET parts 2 query)
    string(REPLACE "~" ";" build "${build}")
    string(REPLACE "~" ";" query "${query}")
    run_cli(0 index build --out "${WORK_DIR}/${name}" ${build})
    require_fragment("${cli_output}" "Index built successfully." "${name} build")
    run_cli(0 search --index "${WORK_DIR}/${name}" ${query})
    set(output "${cli_output}")
    string(REPLACE "\r\n" "\n" output "${output}")
    string(REGEX REPLACE "[^\n]*time \\(ms\\):[^\n]*\n" "" output "${output}")
    string(REGEX REPLACE "Index load time scope:[^\n]*\n" "" output "${output}")
    file(READ "${GOLDEN_DIR}/${name}.txt" expected)
    if(NOT output STREQUAL expected)
        file(WRITE "${WORK_DIR}/${name}.actual.txt" "${output}")
        list(APPEND mismatches "${name} (actual: ${WORK_DIR}/${name}.actual.txt)")
    endif()
endforeach()
if(mismatches)
    string(REPLACE ";" "\n  " mismatches "${mismatches}")
    message(FATAL_ERROR "Saved-index output differs from the one-shot golden output:\n  ${mismatches}")
endif()

# A saved index reports its load time.
set(H "${WORK_DIR}/hybrid_q002")
run_cli(0 search --index "${H}" --query "${D}/hybrid/queries.jsonl" --query-id Q002)
require_fragment("${cli_output}" "Index load time (ms):" "load time")

# Batch queries on a saved index.
run_cli(0 search --index "${H}" --query "${D}/hybrid/queries.jsonl" --all-queries --query-threads 2)
require_fragment("${cli_output}" "Batch queries:" "batch")

# Quantized hybrid index without FP32 rows.
run_cli(0 index build --out "${WORK_DIR}/sq8" --mode hybrid --quantization sq8 --drop-fp32
    --dataset "${D}/hybrid/corpus.jsonl")
run_cli(0 search --index "${WORK_DIR}/sq8" --query "${D}/hybrid/queries.jsonl" --query-id Q002)
require_fragment("${cli_output}" "FP32 rows released" "sq8")

# Misplaced options are argument errors (exit 2).
run_cli(2 search --index "${H}" --m 8 --query "${D}/hybrid/queries.jsonl" --query-id Q002)
require_fragment("${cli_errors}" "fixed when the index is built" "build option at query time")
run_cli(2 index build --out "${WORK_DIR}/bad" --mode hybrid --dataset "${D}/hybrid/corpus.jsonl"
    --k 5)
require_fragment("${cli_errors}" "is a query option" "query option at build time")
run_cli(2 search --index "${H}" --bm25-strategy exhaustive --query "${D}/hybrid/queries.jsonl"
    --query-id Q002)
require_fragment("${cli_errors}" "compressed postings" "strategy needing another layout")
run_cli(2 search --index "${WORK_DIR}/lexical_q002" --alpha 0.5 --query "${D}/queries.jsonl"
    --query-id Q002)
require_fragment("${cli_errors}" "require a hybrid index" "fusion option on a lexical index")
run_cli(2 search --index "${WORK_DIR}/lexical_q002" --ef-search 10 --query "${D}/queries.jsonl"
    --query-id Q002)
run_cli(2 index build --out "${H}" --mode hybrid --dataset "${D}/hybrid/corpus.jsonl")
require_fragment("${cli_errors}" "already exists" "non-empty output directory")
run_cli(2 index build --mode hybrid --dataset "${D}/hybrid/corpus.jsonl")

# A compressed BM25 index accepts another compressed strategy at query time.
run_cli(0 search --index "${WORK_DIR}/lexical_q002" --bm25-strategy wand --query "${D}/queries.jsonl"
    --query-id Q002 --k 5)

# info and verify.
run_cli(0 index info --index "${H}")
require_fragment("${cli_output}" "Search mode: Hybrid" "info")
run_cli(0 index verify --index "${H}")
require_fragment("${cli_output}" "Index verified" "verify")

# Damaged or missing files are input-data errors (exit 3).
run_cli(3 search --index "${WORK_DIR}/missing" --query "${D}/hybrid/queries.jsonl" --query-id Q002)
file(GLOB snapshot_dirs "${WORK_DIR}/hybrid_q002_rrf/snapshot-*")
file(APPEND "${snapshot_dirs}/bm25.sec" "x")
run_cli(3 search --index "${WORK_DIR}/hybrid_q002_rrf" --query "${D}/hybrid/queries.jsonl"
    --query-id Q002)
require_fragment("${cli_errors}" "differs from the manifest" "size mismatch")
file(REMOVE "${snapshot_dirs}/bm25.sec")
run_cli(3 index verify --index "${WORK_DIR}/hybrid_q002_rrf")

list(LENGTH cases count)
message(STATUS "Saved-index outputs: ${count} golden cases identical; index command errors checked.")
