if(NOT DEFINED CONFIG_EXE OR NOT DEFINED TEMPLATE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "CONFIG_EXE, TEMPLATE and TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(READ "${TEMPLATE}" template_json)

set(old_settings "${TEST_ROOT}/old-settings.json")
string(REGEX REPLACE
    "[ \t]*\"document_catalog_storage\"[ \t]*:[ \t]*\"memory\"[ \t]*,?\r?\n"
    ""
    old_json
    "${template_json}")
file(WRITE "${old_settings}" "${old_json}")

execute_process(
    COMMAND "${CONFIG_EXE}" inspect --settings "${old_settings}"
    RESULT_VARIABLE old_rc
    OUTPUT_VARIABLE old_output
    ERROR_VARIABLE old_error
    TIMEOUT 15)
if(NOT old_rc EQUAL 0 OR
   NOT old_output MATCHES "document_catalog_storage=memory")
    message(FATAL_ERROR
        "old Settings.json did not default to memory: rc=${old_rc} "
        "out=${old_output} err=${old_error}")
endif()

foreach(storage IN ITEMS memory sqlite)
    set(output "${TEST_ROOT}/configured-${storage}.json")
    execute_process(
        COMMAND "${CONFIG_EXE}" configure
            --template "${TEMPLATE}"
            --output "${output}"
            --port 15001
            --year 2026
            --threads 2
            --file-timeout 120
            --prm-autodetect 1
            --document-catalog-storage "${storage}"
            --quiet
        RESULT_VARIABLE configure_rc
        OUTPUT_VARIABLE configure_output
        ERROR_VARIABLE configure_error
        TIMEOUT 15)
    if(NOT configure_rc EQUAL 0)
        message(FATAL_ERROR
            "configure ${storage} failed: ${configure_error}")
    endif()

    execute_process(
        COMMAND "${CONFIG_EXE}" inspect --settings "${output}"
        RESULT_VARIABLE inspect_rc
        OUTPUT_VARIABLE inspect_output
        ERROR_VARIABLE inspect_error
        TIMEOUT 15)
    if(NOT inspect_rc EQUAL 0 OR
       NOT inspect_output MATCHES "document_catalog_storage=${storage}")
        message(FATAL_ERROR
            "inspect ${storage} failed: rc=${inspect_rc} "
            "out=${inspect_output} err=${inspect_error}")
    endif()

    execute_process(
        COMMAND "${CONFIG_EXE}" validate --settings "${output}"
        RESULT_VARIABLE validate_rc
        OUTPUT_VARIABLE validate_output
        ERROR_VARIABLE validate_error
        TIMEOUT 15)
    if(NOT validate_rc EQUAL 0 OR
       NOT validate_output MATCHES "settings_valid=1")
        message(FATAL_ERROR
            "non-interactive validate ${storage} failed: rc=${validate_rc} "
            "out=${validate_output} err=${validate_error}")
    endif()
endforeach()

# The interactive installer path must always ask the question. Empty input
# keeps memory for a new install and keeps sqlite from imported Settings.json.
file(WRITE "${TEST_ROOT}/interactive-input.txt" "\n\n\n\n\n\n")
foreach(case IN ITEMS new imported)
    set(interactive_output "${TEST_ROOT}/interactive-${case}.json")
    set(interactive_args
        configure-interactive
        --template "${TEMPLATE}"
        --output "${interactive_output}"
        --language en)
    if(case STREQUAL "imported")
        list(APPEND interactive_args
            --import-settings "${TEST_ROOT}/configured-sqlite.json")
        set(expected_storage sqlite)
    else()
        set(expected_storage memory)
    endif()
    execute_process(
        COMMAND "${CONFIG_EXE}" ${interactive_args}
        INPUT_FILE "${TEST_ROOT}/interactive-input.txt"
        RESULT_VARIABLE interactive_rc
        OUTPUT_VARIABLE interactive_stdout
        ERROR_VARIABLE interactive_stderr
        TIMEOUT 15)
    if(NOT interactive_rc EQUAL 0 OR
       NOT interactive_stdout MATCHES
           "Where should the document catalog")
        message(FATAL_ERROR
            "interactive ${case} did not ask the catalog question: "
            "rc=${interactive_rc} out=${interactive_stdout} "
            "err=${interactive_stderr}")
    endif()
    execute_process(
        COMMAND "${CONFIG_EXE}" inspect --settings "${interactive_output}"
        RESULT_VARIABLE interactive_inspect_rc
        OUTPUT_VARIABLE interactive_inspect_output
        ERROR_VARIABLE interactive_inspect_error
        TIMEOUT 15)
    if(NOT interactive_inspect_rc EQUAL 0 OR
       NOT interactive_inspect_output MATCHES
           "document_catalog_storage=${expected_storage}")
        message(FATAL_ERROR
            "interactive ${case} did not keep ${expected_storage}: "
            "rc=${interactive_inspect_rc} out=${interactive_inspect_output} "
            "err=${interactive_inspect_error}")
    endif()
endforeach()

set(invalid "${TEST_ROOT}/invalid.json")
string(REPLACE
    "\"document_catalog_storage\": \"memory\""
    "\"document_catalog_storage\": \"invalid\""
    invalid_json
    "${template_json}")
file(WRITE "${invalid}" "${invalid_json}")
execute_process(
    COMMAND "${CONFIG_EXE}" validate --settings "${invalid}"
    RESULT_VARIABLE invalid_rc
    OUTPUT_VARIABLE invalid_output
    ERROR_VARIABLE invalid_error
    TIMEOUT 15)
if(invalid_rc EQUAL 0 OR
   NOT invalid_output MATCHES
       "config.document_catalog_storage must be memory or sqlite")
    message(FATAL_ERROR
        "invalid document catalog storage was accepted: rc=${invalid_rc} "
        "out=${invalid_output} err=${invalid_error}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
