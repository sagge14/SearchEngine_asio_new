if(NOT DEFINED CONFIG_EXE OR NOT DEFINED TEMPLATE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "CONFIG_EXE, TEMPLATE and TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(READ "${TEMPLATE}" template_json)

function(searchengine_validate_autopad_case case_name prm_value prd_value)
    set(case_json "${template_json}")
    string(REGEX REPLACE
        "\"prm_base_dir\"[ \t]*:[ \t]*\"[^\"]*\""
        "\"prm_base_dir\": \"${prm_value}\""
        case_json
        "${case_json}")
    string(REGEX REPLACE
        "\"prd_base_dir\"[ \t]*:[ \t]*\"[^\"]*\""
        "\"prd_base_dir\": \"${prd_value}\""
        case_json
        "${case_json}")

    set(settings_path "${TEST_ROOT}/${case_name}.json")
    file(WRITE "${settings_path}" "${case_json}")

    execute_process(
        COMMAND "${CONFIG_EXE}" validate --settings "${settings_path}"
        RESULT_VARIABLE validate_rc
        OUTPUT_VARIABLE validate_output
        ERROR_VARIABLE validate_error
        TIMEOUT 15)
    if(NOT validate_rc EQUAL 0 OR
       NOT validate_output MATCHES "settings_valid=1")
        message(FATAL_ERROR
            "AutoPad contract case ${case_name} failed: rc=${validate_rc} "
            "out=${validate_output} err=${validate_error}")
    endif()
    if(validate_output MATCHES "must be a non-empty string")
        message(FATAL_ERROR
            "AutoPad contract case ${case_name} still requires non-empty paths: "
            "${validate_output}")
    endif()
endfunction()

searchengine_validate_autopad_case(both_empty "" "")
searchengine_validate_autopad_case(prm_empty "" "D:/BASES_PRD")
searchengine_validate_autopad_case(prd_empty "D:/BASES" "")
searchengine_validate_autopad_case(both_set "D:/BASES" "D:/BASES_PRD")

file(REMOVE_RECURSE "${TEST_ROOT}")
