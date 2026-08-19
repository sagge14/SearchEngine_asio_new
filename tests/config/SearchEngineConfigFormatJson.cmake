if(NOT DEFINED CONFIG_EXE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "CONFIG_EXE and TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")

macro(run_format_json label settings_file expect_success)
    execute_process(
        COMMAND "${CONFIG_EXE}" format-json --settings "${settings_file}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE  _err
        TIMEOUT 15)
    if("${expect_success}" STREQUAL "TRUE")
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR
                "format-json should PASS for '${label}' but failed (rc=${_rc})\n"
                "stdout: ${_out}\nstderr: ${_err}")
        endif()
    else()
        if(_rc EQUAL 0)
            message(FATAL_ERROR
                "format-json should FAIL for '${label}' but succeeded\n"
                "stdout: ${_out}\nstderr: ${_err}")
        endif()
    endif()
endmacro()

macro(run_compare_json label left_file right_file expect_equal)
    execute_process(
        COMMAND "${CONFIG_EXE}" compare-json --left "${left_file}" --right "${right_file}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE  _err
        TIMEOUT 15)
    if("${expect_equal}" STREQUAL "TRUE")
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR
                "compare-json should be equal for '${label}' but differ (rc=${_rc})\n"
                "stdout: ${_out}\nstderr: ${_err}")
        endif()
    else()
        if(_rc EQUAL 0)
            message(FATAL_ERROR
                "compare-json should differ for '${label}' but reported equal\n"
                "stdout: ${_out}\nstderr: ${_err}")
        endif()
    endif()
endmacro()

# ------------------------------------------------------------------
# 1. Compact JSON -> pretty JSON
# ------------------------------------------------------------------
set(compact_json
    "{\"config\":{\"Name\":\"test\",\"dirs\":[\"D:\\\\A\",\"D:\\\\B\"],\"exact_search\":true},\"Files\":[]}"
)
set(compact_file "${TEST_ROOT}/compact.json")
file(WRITE "${compact_file}" "${compact_json}")

run_format_json("compact to pretty" "${compact_file}" TRUE)

file(READ "${compact_file}" pretty_content)
if(NOT pretty_content MATCHES "\n")
    message(FATAL_ERROR "format-json did not introduce newlines")
endif()
if(NOT pretty_content MATCHES "    ")
    message(FATAL_ERROR "format-json did not introduce 4-space indent")
endif()

# ------------------------------------------------------------------
# 2. Semantic equality
# ------------------------------------------------------------------
set(semantic_original "${TEST_ROOT}/semantic-original.json")
file(WRITE "${semantic_original}" "${compact_json}")

set(semantic_work "${TEST_ROOT}/semantic-work.json")
file(READ "${semantic_original}" _semantic_copy)
file(WRITE "${semantic_work}" "${_semantic_copy}")

run_format_json("semantic work copy" "${semantic_work}" TRUE)
run_compare_json("semantic equality" "${semantic_original}" "${semantic_work}" TRUE)

# ------------------------------------------------------------------
# 3. Unknown fields preserved
# ------------------------------------------------------------------
set(unknown_json
    "{\"custom_unknown_field\":{\"x\":123},\"config\":{\"Name\":\"test\"}}"
)
set(unknown_original "${TEST_ROOT}/unknown-original.json")
file(WRITE "${unknown_original}" "${unknown_json}")

set(unknown_work "${TEST_ROOT}/unknown-work.json")
file(READ "${unknown_original}" _unknown_copy)
file(WRITE "${unknown_work}" "${_unknown_copy}")

run_format_json("unknown fields" "${unknown_work}" TRUE)
run_compare_json("unknown fields preserved" "${unknown_original}" "${unknown_work}" TRUE)

file(READ "${unknown_work}" unknown_formatted)
if(NOT unknown_formatted MATCHES "custom_unknown_field")
    message(FATAL_ERROR "unknown field name missing after format-json")
endif()
if(NOT unknown_formatted MATCHES "123")
    message(FATAL_ERROR "unknown field value missing after format-json")
endif()

# ------------------------------------------------------------------
# 4. Cyrillic preserved
# ------------------------------------------------------------------
set(cyrillic_json
    "{\"config\":{\"custom_path\":\"D:\\\\РАЗНОЕ\\\\Документы\"}}"
)
set(cyrillic_original "${TEST_ROOT}/cyrillic-original.json")
file(WRITE "${cyrillic_original}" "${cyrillic_json}")

set(cyrillic_work "${TEST_ROOT}/cyrillic-work.json")
file(READ "${cyrillic_original}" _cyrillic_copy)
file(WRITE "${cyrillic_work}" "${_cyrillic_copy}")

run_format_json("cyrillic" "${cyrillic_work}" TRUE)
run_compare_json("cyrillic semantic equality" "${cyrillic_original}" "${cyrillic_work}" TRUE)

file(READ "${cyrillic_work}" cyrillic_formatted)
if(NOT cyrillic_formatted MATCHES "РАЗНОЕ")
    message(FATAL_ERROR "cyrillic path fragment missing after format-json")
endif()

# ------------------------------------------------------------------
# 5. Invalid JSON is non-destructive
# ------------------------------------------------------------------
set(invalid_json "{\"config\":")
set(invalid_file "${TEST_ROOT}/invalid.json")
file(WRITE "${invalid_file}" "${invalid_json}")

file(READ "${invalid_file}" invalid_before)
string(LENGTH "${invalid_before}" invalid_size_before)

run_format_json("invalid json" "${invalid_file}" FALSE)

file(READ "${invalid_file}" invalid_after)
string(LENGTH "${invalid_after}" invalid_size_after)

if(NOT invalid_after STREQUAL "${invalid_before}")
    message(FATAL_ERROR
        "invalid JSON file content changed after failed format-json\n"
        "before: '${invalid_before}'\nafter: '${invalid_after}'")
endif()
if(NOT invalid_size_after EQUAL invalid_size_before)
    message(FATAL_ERROR
        "invalid JSON file size changed after failed format-json "
        "(${invalid_size_before} -> ${invalid_size_after})")
endif()

# ------------------------------------------------------------------
# 6. Formatting idempotence
# ------------------------------------------------------------------
set(idempotent_file "${TEST_ROOT}/idempotent.json")
file(WRITE "${idempotent_file}" "${compact_json}")

run_format_json("idempotent first pass" "${idempotent_file}" TRUE)
file(READ "${idempotent_file}" idempotent_once)

run_format_json("idempotent second pass" "${idempotent_file}" TRUE)
file(READ "${idempotent_file}" idempotent_twice)

if(NOT idempotent_twice STREQUAL "${idempotent_once}")
    message(FATAL_ERROR "format-json is not idempotent on already formatted JSON")
endif()

message(STATUS "SearchEngineConfigFormatJson: all checks passed")
