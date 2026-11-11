cmake_minimum_required(VERSION 3.20)
# Market comparison: --analyzer and --bm25-query-weighting through the executable. Each is
# reported only when not the default, english-v1 stems queries like documents, and invalid
# use exits 2.
set(D "${SOURCE_DIR}/datasets")

function(run_cli expected)
    execute_process(COMMAND "${CLI_PATH}" ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 60)
    if(NOT "${status}" STREQUAL "${expected}")
        message(FATAL_ERROR "Expected ${expected}, got ${status}: ${ARGN}\n${output}\n${errors}")
    endif()
    set(cli_output "${output}" PARENT_SCOPE)
endfunction()

function(result_rows out)
    string(REGEX MATCHALL "\n\\| +[0-9]+ \\| +D[0-9]+ \\|[^\n]*" rows "${cli_output}")
    set(${out} "${rows}" PARENT_SCOPE)
endfunction()

set(L search --mode lexical --dataset "${D}/hybrid/corpus.jsonl")

run_cli(0 ${L} --query-text "vecinos cercanos")
if(cli_output MATCHES "BM25 analyzer" OR cli_output MATCHES "BM25 query weighting")
    message(FATAL_ERROR "Default analyzer/weighting must not be reported\n${cli_output}")
endif()

# Inflected and stemmed English queries give the same ranking under english-v1.
run_cli(0 ${L} --analyzer english-v1 --query-text "vectors indexed")
if(NOT cli_output MATCHES "\nBM25 analyzer: english-v1\n")
    message(FATAL_ERROR "english-v1 not reported\n${cli_output}")
endif()
result_rows(inflected)
run_cli(0 ${L} --analyzer english-v1 --bm25-strategy bmw --query-text "vector index")
result_rows(stemmed)
if(inflected STREQUAL "" OR NOT inflected STREQUAL stemmed)
    message(FATAL_ERROR "Stemmed query ranking differs\n${inflected}\n---\n${stemmed}")
endif()

run_cli(0 search --mode hybrid --dataset "${D}/hybrid/corpus.jsonl"
    --query "${D}/hybrid/queries.jsonl" --query-id Q001 --analyzer english-v1)
if(NOT cli_output MATCHES "\nBM25 analyzer: english-v1\n")
    message(FATAL_ERROR "english-v1 not reported in hybrid mode\n${cli_output}")
endif()

run_cli(2 ${L} --analyzer porter --query-text "graph")
run_cli(2 ${L} --analyzer english-v1 --analyzer english-v1 --query-text "graph")
run_cli(2 search --algorithm hnsw --dataset "${D}/hybrid/corpus.jsonl"
    --query "${D}/hybrid/queries.jsonl" --query-id Q001 --analyzer english-v1)

# Query weighting: frequency (the default, not reported) counts a repeated term again;
# unique ignores the repetition and is reported.
run_cli(0 ${L} --query-text "vector graph")
result_rows(single)
if(cli_output MATCHES "BM25 query weighting")
    message(FATAL_ERROR "default weighting reported\n${cli_output}")
endif()
run_cli(0 ${L} --bm25-query-weighting unique --query-text "vector vector graph")
if(NOT cli_output MATCHES "\nBM25 query weighting: unique\n")
    message(FATAL_ERROR "unique weighting not reported\n${cli_output}")
endif()
result_rows(repeated_unique)
if(single STREQUAL "" OR NOT single STREQUAL repeated_unique)
    message(FATAL_ERROR "unique weighting counted a repetition\n${single}\n---\n${repeated_unique}")
endif()
run_cli(0 ${L} --bm25-strategy bmw --query-text "vector vector graph")
result_rows(repeated_default)
run_cli(0 ${L} --bm25-query-weighting frequency --bm25-strategy bmw
    --query-text "vector vector graph")
if(cli_output MATCHES "BM25 query weighting")
    message(FATAL_ERROR "explicit default weighting reported\n${cli_output}")
endif()
result_rows(repeated_frequency)
if(repeated_frequency STREQUAL "" OR repeated_frequency STREQUAL single
        OR NOT repeated_frequency STREQUAL repeated_default)
    message(FATAL_ERROR "frequency weighting is not the default or had no effect\n${repeated_frequency}")
endif()
run_cli(0 search --mode hybrid --dataset "${D}/hybrid/corpus.jsonl"
    --query "${D}/hybrid/queries.jsonl" --query-id Q001 --bm25-query-weighting unique)
if(NOT cli_output MATCHES "\nBM25 query weighting: unique\n")
    message(FATAL_ERROR "unique weighting not reported in hybrid mode\n${cli_output}")
endif()
run_cli(2 ${L} --bm25-query-weighting linear --query-text "graph")
run_cli(2 ${L} --bm25-query-weighting frequency --bm25-query-weighting unique --query-text "graph")
run_cli(2 search --algorithm hnsw --dataset "${D}/hybrid/corpus.jsonl"
    --query "${D}/hybrid/queries.jsonl" --query-id Q001 --bm25-query-weighting frequency)
