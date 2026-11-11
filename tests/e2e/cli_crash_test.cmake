cmake_minimum_required(VERSION 3.20)
# Crash recovery. The CLI is killed (std::_Exit through AETHER_CRASH_AT) at points inside
# the write-ahead log and the checkpoint. After each crash the index must open and answer
# exactly like a crash-free index holding a prefix of the operations that contains every
# acknowledged one (an operation is acknowledged when its command exits 0).
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(D "${SOURCE_DIR}/datasets")
set(CRASH_STATUS 86)

# Inputs for the operations.
file(READ "${D}/hybrid/corpus.jsonl" corpus)
string(FIND "${corpus}" "\n" newline)
string(SUBSTRING "${corpus}" 0 ${newline} first)
string(REPLACE "\"D001\"" "\"D999\"" added "${first}")
file(WRITE "${WORK_DIR}/new.jsonl" "${added}\n")
string(REPLACE "HNSW" "HNSW actualizado" updated "${first}")
file(WRITE "${WORK_DIR}/update.jsonl" "${updated}\n")

# Operation k (1-based) as CLI arguments after "index".
set(op1 "delete;--id;D002")
set(op2 "add;--dataset;${WORK_DIR}/new.jsonl")
set(op3 "delete;--id;D019")
set(op4 "add;--dataset;${WORK_DIR}/update.jsonl;--replace")

function(cli expected)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${CRASH_ENV} "${CLI_PATH}" ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 120)
    if(NOT status EQUAL expected)
        string(REPLACE ";" " " shown "${ARGN}")
        message(FATAL_ERROR "Expected exit ${expected}, got ${status}: ${shown}\n${output}\n${errors}")
    endif()
    set(cli_output "${output}" PARENT_SCOPE)
endfunction()

function(build dir)
    set(CRASH_ENV "")
    cli(0 index build --out "${dir}" --mode hybrid --dataset "${D}/hybrid/corpus.jsonl")
endfunction()

function(apply dir k)
    set(CRASH_ENV "")
    set(arguments "${op${k}}")
    list(INSERT arguments 1 --index "${dir}")
    cli(0 index ${arguments})
endfunction()

# What an index answers, without run-dependent timing lines.
function(fingerprint dir out)
    set(CRASH_ENV "")
    cli(0 search --index "${dir}" --query "${D}/hybrid/queries.jsonl" --all-queries --k 10)
    string(REGEX REPLACE "[^\n]*(\\(ms\\)|Throughput|load time scope)[^\n]*\n" "" text "${cli_output}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

function(require_state dir expected_name context)
    fingerprint("${dir}" actual)
    if(NOT actual STREQUAL "${${expected_name}}")
        file(WRITE "${WORK_DIR}/${context}.actual.txt" "${actual}")
        file(WRITE "${WORK_DIR}/${context}.expected.txt" "${${expected_name}}")
        message(FATAL_ERROR "${context}: state differs from ${expected_name} (see ${WORK_DIR})")
    endif()
endfunction()

# References R0..R4: crash-free states after 0..4 operations, and R4 compacted.
build("${WORK_DIR}/reference")
fingerprint("${WORK_DIR}/reference" R0)
foreach(k RANGE 1 4)
    apply("${WORK_DIR}/reference" ${k})
    fingerprint("${WORK_DIR}/reference" R${k})
endforeach()
set(CRASH_ENV "")
cli(0 index compact --index "${WORK_DIR}/reference")
fingerprint("${WORK_DIR}/reference" RC)
if(R1 STREQUAL R0 OR R2 STREQUAL R1 OR R3 STREQUAL R2 OR R4 STREQUAL R3)
    message(FATAL_ERROR "Every operation must change the fingerprint for this test to be meaningful.")
endif()

# A) A torn log record: the crashed operation is lost, earlier ones are kept, and the
#    next writer truncates the tail and carries on.
set(I "${WORK_DIR}/torn")
build("${I}")
apply("${I}" 1)
apply("${I}" 2)
set(CRASH_ENV "AETHER_CRASH_AT=wal-torn-record")
set(arguments "${op3}")
list(INSERT arguments 1 --index "${I}")
cli(${CRASH_STATUS} index ${arguments})
require_state("${I}" R2 "torn-record")
set(CRASH_ENV "")
cli(0 index info --index "${I}")
string(FIND "${cli_output}" "incomplete record from a crash was ignored" found)
if(found EQUAL -1)
    message(FATAL_ERROR "info must report the discarded log tail:\n${cli_output}")
endif()
apply("${I}" 3)
apply("${I}" 4)
require_state("${I}" R4 "after-torn-record")

# B) A crash after the record was written, before it was synced: a process crash keeps
#    the page cache, so the operation survives (a power loss could lose it; it was never
#    acknowledged either way).
set(I "${WORK_DIR}/before-sync")
build("${I}")
apply("${I}" 1)
set(CRASH_ENV "AETHER_CRASH_AT=wal-before-sync")
set(arguments "${op2}")
list(INSERT arguments 1 --index "${I}")
cli(${CRASH_STATUS} index ${arguments})
fingerprint("${I}" state)
if(NOT state STREQUAL R2 AND NOT state STREQUAL R1)
    message(FATAL_ERROR "before-sync: the state is not a prefix of the operations")
endif()
require_state("${I}" R2 "before-sync")

# C) A crash in a checkpoint before CURRENT moves: the old snapshot plus the log.
set(I "${WORK_DIR}/checkpoint-current")
build("${I}")
apply("${I}" 1)
apply("${I}" 2)
set(CRASH_ENV "AETHER_CRASH_AT=checkpoint-before-current")
cli(${CRASH_STATUS} index checkpoint --index "${I}")
require_state("${I}" R2 "checkpoint-before-current")
apply("${I}" 3)
set(CRASH_ENV "")
cli(0 index checkpoint --index "${I}")
require_state("${I}" R3 "checkpoint-after-recovery")

# D) A crash in a checkpoint after CURRENT moved but before the log was emptied: replay
#    skips the records the new snapshot already holds.
set(I "${WORK_DIR}/checkpoint-log")
build("${I}")
apply("${I}" 1)
apply("${I}" 2)
set(CRASH_ENV "AETHER_CRASH_AT=checkpoint-before-log-reset")
cli(${CRASH_STATUS} index checkpoint --index "${I}")
require_state("${I}" R2 "checkpoint-before-log-reset")
apply("${I}" 3)
apply("${I}" 4)
require_state("${I}" R4 "log-after-new-snapshot")

# E) A crash in the checkpoint of a compaction: the logged compaction is replayed.
set(I "${WORK_DIR}/compact")
build("${I}")
foreach(k RANGE 1 4)
    apply("${I}" ${k})
endforeach()
set(CRASH_ENV "AETHER_CRASH_AT=checkpoint-before-current")
cli(${CRASH_STATUS} index compact --index "${I}")
require_state("${I}" RC "compact-crash")

message(STATUS "Crash recovery: torn record, unsynced record, and both checkpoint steps recover a prefix with every acknowledged operation.")
