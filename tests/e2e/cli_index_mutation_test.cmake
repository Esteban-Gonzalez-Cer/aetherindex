cmake_minimum_required(VERSION 3.20)
# index add / delete / compact through the CLI: deleted records disappear from results,
# added ones appear, existing IDs need --replace, compaction drops the tombstones, and
# every change survives in the saved index.
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(D "${SOURCE_DIR}/datasets")

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

function(require_no_fragment text fragment context)
    string(FIND "${text}" "${fragment}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR "${context}: unexpected '${fragment}' in:\n${text}")
    endif()
endfunction()

# ----- Hybrid -----------------------------------------------------------------------------
set(H "${WORK_DIR}/hybrid")
run_cli(0 index build --out "${H}" --mode hybrid --dataset "${D}/hybrid/corpus.jsonl")
set(QUERY --query "${D}/hybrid/queries.jsonl" --query-id Q002 --k 30)
run_cli(0 search --index "${H}" ${QUERY})
require_fragment("${cli_output}" " D002 |" "D002 before delete")

run_cli(0 index delete --index "${H}" --id D002 --id D019)
require_fragment("${cli_output}" "Records deleted: 2" "delete count")
run_cli(0 search --index "${H}" ${QUERY})
require_no_fragment("${cli_output}" " D002 |" "D002 after delete")
require_no_fragment("${cli_output}" " D019 |" "D019 after delete")
require_fragment("${cli_output}" "Documents indexed: 28" "live count after delete")
run_cli(3 index delete --index "${H}" --id D002)
require_fragment("${cli_errors}" "Unknown document ID" "deleting twice")

# A new document copied from D001 under a new ID, and D001 itself (needs --replace).
# Read the first line as text: a CMake list would split the JSON at its semicolons.
file(READ "${D}/hybrid/corpus.jsonl" corpus)
string(FIND "${corpus}" "\n" newline)
string(SUBSTRING "${corpus}" 0 ${newline} first)
string(REPLACE "\"D001\"" "\"D999\"" added "${first}")
file(WRITE "${WORK_DIR}/new.jsonl" "${added}\n")
run_cli(0 index add --index "${H}" --dataset "${WORK_DIR}/new.jsonl")
require_fragment("${cli_output}" "Records added: 1" "add count")
file(WRITE "${WORK_DIR}/again.jsonl" "${first}\n")
run_cli(3 index add --index "${H}" --dataset "${WORK_DIR}/again.jsonl")
require_fragment("${cli_errors}" "--replace" "existing ID without --replace")
run_cli(0 index add --index "${H}" --dataset "${WORK_DIR}/again.jsonl" --replace)
require_fragment("${cli_output}" "Records replaced: 1" "replace count")
run_cli(0 search --index "${H}" --query "${D}/hybrid/queries.jsonl" --query-id Q001 --k 30)
require_fragment("${cli_output}" " D999 |" "added document")
require_fragment("${cli_output}" "Documents indexed: 29" "live count after add")

run_cli(0 index info --index "${H}")
run_cli(0 index verify --index "${H}")
require_fragment("${cli_output}" "segment(s)" "verify shows BM25 segments")
run_cli(0 index compact --index "${H}")
require_fragment("${cli_output}" "Deleted records held: 0" "compaction")
run_cli(0 search --index "${H}" --query "${D}/hybrid/queries.jsonl" --query-id Q001 --k 30)
require_fragment("${cli_output}" " D999 |" "added document after compaction")
require_no_fragment("${cli_output}" " D002 |" "deleted document after compaction")
run_cli(0 index verify --index "${H}")
require_fragment("${cli_output}" "1 segment(s)" "one segment after compaction")

# ----- Vectors from CSV ---------------------------------------------------------------------
set(V "${WORK_DIR}/vectors")
run_cli(0 index build --out "${V}" --algorithm hnsw --dataset "${D}/examples/vectors.csv")
set(VQUERY --query "${D}/examples/query.csv" --k 10)
run_cli(0 index delete --index "${V}" --id 10)
run_cli(0 search --index "${V}" ${VQUERY})
require_no_fragment("${cli_output}" "|        10 |" "vector 10 after delete")
file(WRITE "${WORK_DIR}/vector10.csv" "10,1.0,0.0\n")
run_cli(0 index add --index "${V}" --dataset "${WORK_DIR}/vector10.csv")
run_cli(0 search --index "${V}" ${VQUERY})
require_fragment("${cli_output}" "|    1 |        10 | 0.000000000 |" "vector 10 back first")
run_cli(3 index delete --index "${V}" --id 12345)

# ----- Lexical ------------------------------------------------------------------------------
set(L "${WORK_DIR}/lexical")
run_cli(0 index build --out "${L}" --mode lexical --dataset "${D}/corpus.jsonl")
run_cli(0 index delete --index "${L}" --id D002)
run_cli(0 search --index "${L}" --query "${D}/queries.jsonl" --query-id Q002 --k 10)
require_no_fragment("${cli_output}" " D002 |" "lexical delete")

# ----- Errors -------------------------------------------------------------------------------
run_cli(0 index build --out "${WORK_DIR}/lean" --mode hybrid --quantization sq8 --drop-fp32
    --dataset "${D}/hybrid/corpus.jsonl")
run_cli(2 index add --index "${WORK_DIR}/lean" --dataset "${WORK_DIR}/new.jsonl")
require_fragment("${cli_errors}" "drop-fp32" "adding to an index without FP32 rows")
run_cli(2 index add --index "${V}" --dataset "${WORK_DIR}/new.jsonl")
run_cli(2 index delete --index "${H}")
run_cli(2 index add --index "${H}" --id D001)

message(STATUS "Index add, delete and compact behave as specified.")
