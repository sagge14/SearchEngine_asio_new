cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED CONFIG_EXE OR NOT EXISTS "${CONFIG_EXE}")
    message(FATAL_ERROR "CONFIG_EXE is missing")
endif()
if(NOT DEFINED TEMPLATE OR NOT EXISTS "${TEMPLATE}")
    message(FATAL_ERROR "TEMPLATE is missing")
endif()
if(NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "TEST_ROOT is missing")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
set(OUTPUT_SETTINGS "${TEST_ROOT}/Settings.json")
file(SHA256 "${TEMPLATE}" TEMPLATE_HASH_BEFORE)

execute_process(
    COMMAND "${CONFIG_EXE}" system-info
    RESULT_VARIABLE SYSTEM_RESULT
    OUTPUT_VARIABLE SYSTEM_OUTPUT
    ERROR_VARIABLE SYSTEM_ERROR
)
if(NOT SYSTEM_RESULT EQUAL 0)
    message(FATAL_ERROR "system-info failed: ${SYSTEM_ERROR}")
endif()
if(NOT SYSTEM_OUTPUT MATCHES "current_year=([0-9][0-9][0-9][0-9])")
    message(FATAL_ERROR "system-info did not return current_year: ${SYSTEM_OUTPUT}")
endif()
set(CURRENT_YEAR "${CMAKE_MATCH_1}")

execute_process(
    COMMAND
        "${CONFIG_EXE}" configure-local-machine
        --template "${TEMPLATE}"
        --output "${OUTPUT_SETTINGS}"
    RESULT_VARIABLE CONFIGURE_RESULT
    OUTPUT_VARIABLE CONFIGURE_OUTPUT
    ERROR_VARIABLE CONFIGURE_ERROR
)
if(NOT CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR
        "configure-local-machine failed: ${CONFIGURE_ERROR} ${CONFIGURE_OUTPUT}")
endif()
foreach(REQUIRED_OUTPUT
    "settings_written=1"
    "local_machine=1"
    "document_catalog_storage=sqlite"
    "prm_short_content_autodetect=0"
    "scan_on_startup=0")
    if(NOT CONFIGURE_OUTPUT MATCHES "${REQUIRED_OUTPUT}")
        message(FATAL_ERROR
            "configure-local-machine output is missing ${REQUIRED_OUTPUT}: ${CONFIGURE_OUTPUT}")
    endif()
endforeach()

file(READ "${OUTPUT_SETTINGS}" OUTPUT_JSON)
string(JSON OUTPUT_YEAR GET "${OUTPUT_JSON}" config year)
string(JSON OUTPUT_PORT GET "${OUTPUT_JSON}" config asio_port)
string(JSON OUTPUT_THREADS GET "${OUTPUT_JSON}" config thread_count)
string(JSON OUTPUT_CATALOG GET "${OUTPUT_JSON}" config document_catalog_storage)
string(JSON OUTPUT_PRM GET
    "${OUTPUT_JSON}" config enable_prm_short_content_autodetect)
string(JSON OUTPUT_SCAN GET "${OUTPUT_JSON}" config scan_on_startup)

if(NOT OUTPUT_YEAR STREQUAL CURRENT_YEAR)
    message(FATAL_ERROR
        "local-machine year mismatch: expected ${CURRENT_YEAR}, got ${OUTPUT_YEAR}")
endif()
if(OUTPUT_PORT LESS 1 OR OUTPUT_PORT GREATER 65535)
    message(FATAL_ERROR "local-machine port is invalid: ${OUTPUT_PORT}")
endif()
if(OUTPUT_THREADS LESS 2)
    message(FATAL_ERROR "local-machine thread count is invalid: ${OUTPUT_THREADS}")
endif()
if(NOT OUTPUT_CATALOG STREQUAL "sqlite")
    message(FATAL_ERROR "local-machine catalog must be sqlite")
endif()
if(OUTPUT_PRM)
    message(FATAL_ERROR "local-machine PRM short-content update must be disabled")
endif()
if(OUTPUT_SCAN)
    message(FATAL_ERROR "local-machine scan_on_startup must remain disabled")
endif()

execute_process(
    COMMAND "${CONFIG_EXE}" validate --settings "${OUTPUT_SETTINGS}"
    RESULT_VARIABLE VALIDATE_RESULT
    OUTPUT_VARIABLE VALIDATE_OUTPUT
    ERROR_VARIABLE VALIDATE_ERROR
)
if(NOT VALIDATE_RESULT EQUAL 0)
    message(FATAL_ERROR
        "generated local-machine Settings failed validation: ${VALIDATE_ERROR} ${VALIDATE_OUTPUT}")
endif()

file(SHA256 "${TEMPLATE}" TEMPLATE_HASH_AFTER)
if(NOT TEMPLATE_HASH_BEFORE STREQUAL TEMPLATE_HASH_AFTER)
    message(FATAL_ERROR "configure-local-machine modified the tracked template")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
