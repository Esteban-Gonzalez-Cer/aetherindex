execute_process(
    COMMAND "${CLI_PATH}"
    RESULT_VARIABLE cli_status
    OUTPUT_VARIABLE cli_output
    ERROR_VARIABLE cli_error
    TIMEOUT 10
)

if(NOT cli_status STREQUAL "0")
    message(FATAL_ERROR "CLI failed (${cli_status}): ${cli_error}")
endif()

string(REPLACE "\r\n" "\n" cli_output "${cli_output}")
set(expected_output "=== AetherIndex ===\nBusqueda por fuerza bruta\n\nVector de consulta: 1.2 2.2 3.2 \n\nVecinos mas cercanos:\n1. ID: 1 | Distancia^2: 4.92\n2. ID: 2 | Distancia^2: 5.14\n3. ID: 4 | Distancia^2: 9.77\n")
if(NOT cli_output STREQUAL expected_output OR NOT cli_error STREQUAL "")
    message(FATAL_ERROR "Unexpected CLI output:\n${cli_output}\n${cli_error}")
endif()
