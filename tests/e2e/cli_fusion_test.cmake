cmake_minimum_required(VERSION 3.20)
# Hybrid fusion through the executable: convex with alpha 0.65 is the default, --fusion rrf
# (or --rrf-k / --rrf-weight) selects RRF, W = 0.5 keeps the classic RRF ranking, and
# invalid or contradictory use exits 2.
set(D "${SOURCE_DIR}/datasets/hybrid")

function(run_cli expected)
    execute_process(COMMAND "${CLI_PATH}" ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 60)
    if(NOT "${status}" STREQUAL "${expected}")
        message(FATAL_ERROR "Expected ${expected}, got ${status}: ${ARGN}\n${output}\n${errors}")
    endif()
    set(cli_output "${output}" PARENT_SCOPE)
endfunction()

# Document IDs of the result table, in rank order.
function(result_ids out)
    string(REGEX MATCHALL "\n\\| +[0-9]+ \\| +D[0-9]+ \\|" rows "${cli_output}")
    set(${out} "${rows}" PARENT_SCOPE)
endfunction()

set(H search --mode hybrid --dataset "${D}/corpus.jsonl" --query "${D}/queries.jsonl")

run_cli(0 ${H} --query-id Q001)
if(NOT cli_output MATCHES "\nFusion: convex \\(alpha=0.65, min-max normalized scores\\)\n"
        OR NOT cli_output MATCHES "Fused Score")
    message(FATAL_ERROR "Default fusion is not convex with alpha 0.65\n${cli_output}")
endif()
result_ids(default)
foreach(same IN ITEMS "--alpha;0.65" "--fusion;convex" "--fusion;convex;--alpha;0.65")
    run_cli(0 ${H} --query-id Q001 ${same})
    result_ids(explicit)
    if(default STREQUAL "" OR NOT explicit STREQUAL default)
        message(FATAL_ERROR "${same} differs from the default\n${default}\n---\n${explicit}")
    endif()
endforeach()

run_cli(0 ${H} --query-id Q001 --fusion rrf)
if(NOT cli_output MATCHES "\nFusion: RRF \\(k_rrf=60\\)\n" OR NOT cli_output MATCHES "RRF Score"
        OR NOT cli_output MATCHES "HNSW \\+ BM25 \\+ RRF")
    message(FATAL_ERROR "Classic RRF not reported\n${cli_output}")
endif()
result_ids(classic)
run_cli(0 ${H} --query-id Q001 --rrf-k 60)
result_ids(rrf_k)
if(classic STREQUAL "" OR NOT rrf_k STREQUAL classic)
    message(FATAL_ERROR "--rrf-k did not select RRF\n${classic}\n---\n${rrf_k}")
endif()

run_cli(0 ${H} --query-id Q001 --rrf-weight 0.5)
if(NOT cli_output MATCHES "\nFusion: weighted RRF \\(k_rrf=60, dense weight=0.5, lexical weight=1-dense\\)\n")
    message(FATAL_ERROR "Weighted RRF not reported\n${cli_output}")
endif()
result_ids(half)
if(classic STREQUAL "" OR NOT half STREQUAL classic)
    message(FATAL_ERROR "W = 0.5 changed the RRF ranking\n${classic}\n---\n${half}")
endif()

run_cli(0 ${H} --query-id Q001 --alpha 0.7)
if(NOT cli_output MATCHES "\nFusion: convex \\(alpha=0.7, min-max normalized scores\\)\n"
        OR NOT cli_output MATCHES "Fused Score" OR NOT cli_output MATCHES "HNSW \\+ BM25 \\+ fusion")
    message(FATAL_ERROR "Convex fusion not reported\n${cli_output}")
endif()
result_ids(convex)
if(convex STREQUAL "")
    message(FATAL_ERROR "Convex fusion returned no rows\n${cli_output}")
endif()

run_cli(2 ${H} --query-id Q001 --alpha 1.5)
run_cli(2 ${H} --query-id Q001 --rrf-weight -0.1)
run_cli(2 ${H} --query-id Q001 --alpha 0.5 --rrf-k 30)
run_cli(2 ${H} --query-id Q001 --alpha 0.5 --rrf-weight 0.5)
run_cli(2 ${H} --query-id Q001 --alpha 0.5 --alpha 0.6)
run_cli(2 ${H} --query-id Q001 --fusion linear)
run_cli(2 ${H} --query-id Q001 --fusion rrf --fusion convex)
run_cli(2 ${H} --query-id Q001 --fusion rrf --alpha 0.5)
run_cli(2 ${H} --query-id Q001 --fusion convex --rrf-k 30)
run_cli(2 ${H} --query-id Q001 --rrf-k 30 --fusion convex)
run_cli(2 ${H} --query-id Q001 --fusion convex --rrf-weight 0.5)
run_cli(2 search --mode lexical --dataset "${D}/corpus.jsonl" --query-text graph --fusion rrf)
run_cli(2 search --mode lexical --dataset "${D}/corpus.jsonl" --query-text graph --alpha 0.5)
