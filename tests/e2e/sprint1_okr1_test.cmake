cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/cli_output_checks.cmake")

# The fixture dependency builds the official graph once, measures its 100
# queries, and writes round-trip FP32 CSVs. CLI rebuilds from those same rows.
file(STRINGS "${EVIDENCE_DIR}/query_results.csv" reference_rows)
foreach(algorithm brute-force hnsw)
    set(extra_flags "")
    if(algorithm STREQUAL "hnsw")
        set(extra_flags --m 16 --m0 32 --ef-construction 100 --ef-search 50 --seed 42)
        set(display_algorithm HNSW)
    else()
        set(display_algorithm BruteForce)
    endif()
    execute_process(
        COMMAND "${CLI_PATH}" search
            --dataset "${EVIDENCE_DIR}/fixtures/vectors.csv"
            --query "${EVIDENCE_DIR}/fixtures/queries/query_0.csv"
            --algorithm "${algorithm}" --metric cosine --k 10 ${extra_flags}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 120
    )
    file(WRITE "${EVIDENCE_DIR}/cli_${algorithm}.txt" "${output}")
    file(WRITE "${EVIDENCE_DIR}/cli_${algorithm}.stderr.txt" "${errors}")
    if(NOT "${status}" STREQUAL "0" OR NOT errors STREQUAL "")
        message(FATAL_ERROR "OKR1 CLI ${algorithm} failed: ${status}\n${output}\n${errors}")
    endif()
    string(REPLACE "\r\n" "\n" output "${output}")
    foreach(fragment "Algorithm: ${display_algorithm}\n" "Distance metric: Cosine\n"
                     "Vectors indexed: 1000\n" "Dimensions: 128\n" "Requested K: 10\n"
                     "Results returned: 10\n" "Search completed successfully.")
        if(NOT output MATCHES "${fragment}")
            message(FATAL_ERROR "OKR1 CLI missing '${fragment}': ${output}")
        endif()
    endforeach()
    if(algorithm STREQUAL "hnsw" AND NOT output MATCHES
       "HNSW configuration: M=16 M0=32 efConstruction=100 efSearch=50 seed=42\n")
        message(FATAL_ERROR "CLI must use the frozen OKR1 HNSW configuration.")
    endif()
    check_search_output("${output}" 10)
    set(expected_rows "")
    foreach(reference IN LISTS reference_rows)
        if(reference MATCHES "^0,${algorithm},([0-9]+),([0-9]+),[^,]+,([0-9.]+)$")
            list(APPEND expected_rows "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3}")
        endif()
    endforeach()
    set(actual_rows "")
    foreach(row IN LISTS cli_rows)
        parse_cli_row("${row}")
        list(APPEND actual_rows "${cli_rank},${cli_id},${cli_distance}")
    endforeach()
    if(NOT actual_rows STREQUAL expected_rows)
        message(FATAL_ERROR "CLI differs from the official ${algorithm} ranking/distances.\nActual: ${actual_rows}\nExpected: ${expected_rows}")
    endif()
endforeach()
message(STATUS "OKR1 integrated CLI demonstration: both algorithms match the official query-0 rankings/distances; tables, timing and metadata valid.")
