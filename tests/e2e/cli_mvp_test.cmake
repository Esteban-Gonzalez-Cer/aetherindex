cmake_minimum_required(VERSION 3.20)

include("${CMAKE_CURRENT_LIST_DIR}/cli_output_checks.cmake")

execute_process(
    COMMAND "${GENERATOR_PATH}" "${WORK_DIR}"
    RESULT_VARIABLE status OUTPUT_VARIABLE fixture_output ERROR_VARIABLE errors TIMEOUT 10
)
if(NOT "${status}" STREQUAL "0" OR NOT errors STREQUAL "")
    message(FATAL_ERROR "Could not generate the MVP fixture: ${status}\n${errors}")
endif()
foreach(query_index RANGE 0 9)
    execute_process(
        COMMAND "${CLI_PATH}" search --dataset "${WORK_DIR}/vectors.csv"
                --query "${WORK_DIR}/query_${query_index}.csv" --algorithm brute-force --k 10
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 10
    )
    string(REPLACE "\r\n" "\n" output "${output}")
    if(NOT "${status}" STREQUAL "0" OR NOT errors STREQUAL "")
        message(FATAL_ERROR "MVP query ${query_index} failed: ${status}\n${errors}")
    endif()
    foreach(fragment "Algorithm: BruteForce" "Vectors indexed: 1000" "Dimensions: 128"
                     "Requested K: 10" "Results returned: 10" "Search completed successfully.")
        if(NOT output MATCHES "${fragment}")
            message(FATAL_ERROR "MVP output missing '${fragment}': ${output}")
        endif()
    endforeach()
    check_search_output("${output}" 10)
    math(EXPR expected_first_id "7 * 97 * ${query_index} + 10")
    set(expected_rank 1)
    set(previous_distance -1)
    set(ids "")
    foreach(row IN LISTS cli_rows)
        parse_cli_row("${row}")
        set(rank "${cli_rank}")
        set(id "${cli_id}")
        set(distance "${cli_distance}")
        math(EXPR id_remainder "(${id} - 10) % 7")
        if(NOT rank EQUAL expected_rank OR id LESS 10 OR id GREATER 7003 OR
           NOT id_remainder EQUAL 0 OR id IN_LIST ids OR distance LESS previous_distance OR
           distance LESS 0 OR distance GREATER 2)
            message(FATAL_ERROR "Invalid ID, rank, uniqueness or distance order: ${row}")
        endif()
        if(expected_rank EQUAL 1)
            if(NOT id EQUAL expected_first_id OR NOT distance LESS 0.00001)
                message(FATAL_ERROR "The original embedding must be the nearest result at distance < 1e-5: ${row}")
            endif()
        endif()
        list(APPEND ids "${id}")
        set(previous_distance "${distance}")
        math(EXPR expected_rank "${expected_rank} + 1")
    endforeach()
endforeach()
message(STATUS "1000 x 128 embeddings; seed 42; 10/10 distinct CLI Top-10 queries passed with valid, unique, sorted IDs.")
