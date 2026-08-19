if(NOT DEFINED CONFIG_EXE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "CONFIG_EXE and TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")

macro(run_format_json label settings_file expect_success)
    execute_process(
        COMMAND "${CONFIG_EXE}" format-json --settings "${settings_file}" --line-ending crlf
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

function(read_file_hex file hex_out)
    file(READ "${file}" _hex HEX)
    string(TOLOWER "${_hex}" _hex)
    set(${hex_out} "${_hex}" PARENT_SCOPE)
endfunction()

function(assert_crlf_json_bytes file label)
    read_file_hex("${file}" hex)
    if(hex MATCHES "^efbbbf")
        message(FATAL_ERROR "${label}: unexpected UTF-8 BOM")
    endif()

    string(LENGTH "${hex}" hex_len)
    math(EXPR rem "${hex_len} % 2")
    if(NOT rem EQUAL 0)
        message(FATAL_ERROR "${label}: odd hex length ${hex_len}")
    endif()
    if(hex_len LESS 4)
        message(FATAL_ERROR "${label}: file too short to end with CRLF")
    endif()

    math(EXPR last4_start "${hex_len} - 4")
    string(SUBSTRING "${hex}" ${last4_start} 4 last4)
    if(NOT last4 STREQUAL "0d0a")
        message(FATAL_ERROR "${label}: file does not end with CRLF (last=${last4})")
    endif()

    set(i 0)
    set(crlf_count 0)
    while(i LESS hex_len)
        string(SUBSTRING "${hex}" ${i} 2 byte)
        if(byte STREQUAL "0d")
            math(EXPR next "${i} + 2")
            if(next GREATER_EQUAL hex_len)
                message(FATAL_ERROR "${label}: bare CR at end of file")
            endif()
            string(SUBSTRING "${hex}" ${next} 2 next_byte)
            if(NOT next_byte STREQUAL "0a")
                message(FATAL_ERROR "${label}: bare CR not followed by LF")
            endif()
            math(EXPR crlf_count "${crlf_count} + 1")
            math(EXPR i "${i} + 4")
        elseif(byte STREQUAL "0a")
            message(FATAL_ERROR "${label}: bare LF at hex offset ${i}")
        else()
            math(EXPR i "${i} + 2")
        endif()
    endwhile()

    if(crlf_count LESS 2)
        message(FATAL_ERROR "${label}: expected multiple CRLF, got ${crlf_count}")
    endif()
endfunction()

function(assert_lf_json_bytes file label)
    read_file_hex("${file}" hex)
    string(LENGTH "${hex}" hex_len)
    math(EXPR rem "${hex_len} % 2")
    if(NOT rem EQUAL 0)
        message(FATAL_ERROR "${label}: odd hex length ${hex_len}")
    endif()
    if(hex_len LESS 2)
        message(FATAL_ERROR "${label}: file too short to end with LF")
    endif()

    set(i 0)
    set(lf_count 0)
    while(i LESS hex_len)
        string(SUBSTRING "${hex}" ${i} 2 byte)
        if(byte STREQUAL "0d")
            message(FATAL_ERROR "${label}: unexpected CR in LF mode at hex offset ${i}")
        elseif(byte STREQUAL "0a")
            math(EXPR lf_count "${lf_count} + 1")
        endif()
        math(EXPR i "${i} + 2")
    endwhile()

    if(lf_count LESS 2)
        message(FATAL_ERROR "${label}: expected multiple LF, got ${lf_count}")
    endif()
    math(EXPR last2_start "${hex_len} - 2")
    string(SUBSTRING "${hex}" ${last2_start} 2 last2)
    if(NOT last2 STREQUAL "0a")
        message(FATAL_ERROR "${label}: file does not end with LF (last=${last2})")
    endif()
endfunction()

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
assert_crlf_json_bytes("${compact_file}" "compact to pretty")

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
assert_crlf_json_bytes("${semantic_work}" "semantic equality")

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
assert_crlf_json_bytes("${unknown_work}" "unknown fields")

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
assert_crlf_json_bytes("${cyrillic_work}" "cyrillic")

# ------------------------------------------------------------------
# 5. Invalid JSON is non-destructive
# ------------------------------------------------------------------
set(invalid_json "{\"config\":")
set(invalid_file "${TEST_ROOT}/invalid.json")
file(WRITE "${invalid_file}" "${invalid_json}")

file(READ "${invalid_file}" invalid_before)
string(LENGTH "${invalid_before}" invalid_size_before)
read_file_hex("${invalid_file}" invalid_hex_before)

run_format_json("invalid json" "${invalid_file}" FALSE)

file(READ "${invalid_file}" invalid_after)
string(LENGTH "${invalid_after}" invalid_size_after)
read_file_hex("${invalid_file}" invalid_hex_after)

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
if(NOT invalid_hex_after STREQUAL "${invalid_hex_before}")
    message(FATAL_ERROR
        "invalid JSON file bytes changed after failed format-json")
endif()

# ------------------------------------------------------------------
# 6. Formatting idempotence (byte-for-byte CRLF)
# ------------------------------------------------------------------
set(idempotent_file "${TEST_ROOT}/idempotent.json")
file(WRITE "${idempotent_file}" "${compact_json}")

run_format_json("idempotent first pass" "${idempotent_file}" TRUE)
read_file_hex("${idempotent_file}" idempotent_once)

run_format_json("idempotent second pass" "${idempotent_file}" TRUE)
read_file_hex("${idempotent_file}" idempotent_twice)

if(NOT idempotent_twice STREQUAL "${idempotent_once}")
    message(FATAL_ERROR "format-json is not idempotent on already formatted JSON")
endif()
assert_crlf_json_bytes("${idempotent_file}" "idempotent")

# ------------------------------------------------------------------
# 7. Escaped newline in a JSON string value is preserved
# ------------------------------------------------------------------
set(escaped_json
    "{\"custom\":\"line1\\nline2\",\"config\":{\"Name\":\"test\"}}"
)
set(escaped_original "${TEST_ROOT}/escaped-original.json")
file(WRITE "${escaped_original}" "${escaped_json}")

set(escaped_work "${TEST_ROOT}/escaped-work.json")
file(READ "${escaped_original}" _escaped_copy)
file(WRITE "${escaped_work}" "${_escaped_copy}")

run_format_json("escaped newline" "${escaped_work}" TRUE)
run_compare_json("escaped newline semantic equality" "${escaped_original}" "${escaped_work}" TRUE)
assert_crlf_json_bytes("${escaped_work}" "escaped newline")

read_file_hex("${escaped_work}" escaped_hex)
if(NOT escaped_hex MATCHES "5c6e")
    message(FATAL_ERROR
        "escaped newline: serialized JSON is missing the \\\\n escape sequence")
endif()
if(escaped_hex MATCHES "6c696e65310a6c696e6532")
    message(FATAL_ERROR
        "escaped newline: converter turned JSON string \\\\n into a raw LF")
endif()
if(escaped_hex MATCHES "6c696e65310d0a6c696e6532")
    message(FATAL_ERROR
        "escaped newline: converter turned JSON string \\\\n into a raw CRLF")
endif()

# ------------------------------------------------------------------
# 8. Explicit LF mode keeps bare LF (writer default is not globally CRLF)
# ------------------------------------------------------------------
set(lf_file "${TEST_ROOT}/lf-mode.json")
file(WRITE "${lf_file}" "${compact_json}")
execute_process(
    COMMAND "${CONFIG_EXE}" format-json --settings "${lf_file}" --line-ending lf
    RESULT_VARIABLE _lf_rc
    OUTPUT_VARIABLE _lf_out
    ERROR_VARIABLE  _lf_err
    TIMEOUT 15)
if(NOT _lf_rc EQUAL 0)
    message(FATAL_ERROR
        "format-json --line-ending lf failed (rc=${_lf_rc})\n"
        "stdout: ${_lf_out}\nstderr: ${_lf_err}")
endif()
assert_lf_json_bytes("${lf_file}" "line-ending lf")
run_compare_json("lf mode semantic equality" "${semantic_original}" "${lf_file}" TRUE)

# ------------------------------------------------------------------
# 9. Default format-json (no --line-ending) is CRLF
# ------------------------------------------------------------------
set(default_file "${TEST_ROOT}/default-ending.json")
file(WRITE "${default_file}" "${compact_json}")
execute_process(
    COMMAND "${CONFIG_EXE}" format-json --settings "${default_file}"
    RESULT_VARIABLE _def_rc
    OUTPUT_VARIABLE _def_out
    ERROR_VARIABLE  _def_err
    TIMEOUT 15)
if(NOT _def_rc EQUAL 0)
    message(FATAL_ERROR
        "format-json default line ending failed (rc=${_def_rc})\n"
        "stdout: ${_def_out}\nstderr: ${_def_err}")
endif()
assert_crlf_json_bytes("${default_file}" "default line ending")

message(STATUS "SearchEngineConfigFormatJson: all checks passed")
