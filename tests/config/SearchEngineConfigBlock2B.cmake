if(NOT DEFINED CONFIG_EXE OR NOT DEFINED TEMPLATE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "CONFIG_EXE, TEMPLATE and TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(READ "${TEMPLATE}" template_json)

function(run_configure output_path import_path expected_success)
    set(args
        configure
        --template "${TEMPLATE}"
        --output "${output_path}"
        --port 15001
        --year 2026
        --threads 2
        --file-timeout 120
        --prm-autodetect 1
        --quiet)
    if(NOT "${import_path}" STREQUAL "")
        list(APPEND args --import-settings "${import_path}")
    endif()
    execute_process(
        COMMAND "${CONFIG_EXE}" ${args}
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
        TIMEOUT 20)
    if(expected_success AND NOT rc EQUAL 0)
        message(FATAL_ERROR
            "configure should pass (rc=${rc})\nstdout=${out}\nstderr=${err}")
    elseif(NOT expected_success AND rc EQUAL 0)
        message(FATAL_ERROR "configure should fail for ${import_path}")
    endif()
endfunction()

function(assert_contains path pattern label)
    file(READ "${path}" content)
    if(NOT content MATCHES "${pattern}")
        message(FATAL_ERROR "${label}: pattern '${pattern}' missing")
    endif()
endfunction()

function(assert_not_contains path pattern label)
    file(READ "${path}" content)
    if(content MATCHES "${pattern}")
        message(FATAL_ERROR "${label}: retired pattern '${pattern}' remains")
    endif()
endfunction()

# Fresh template and fresh configure use canonical fields and all-word mode.
assert_contains("${TEMPLATE}" "\"indexed_extensions\"" "fresh template")
assert_contains("${TEMPLATE}" "\"include_extensionless_files\"[ \t]*:[ \t]*true" "fresh template")
assert_contains("${TEMPLATE}" "\"query_word_match\"[ \t]*:[ \t]*\"all\"" "fresh template")
assert_not_contains("${TEMPLATE}" "\"extensions\"" "fresh template")
assert_not_contains("${TEMPLATE}" "\"exact_search\"" "fresh template")

set(fresh_output "${TEST_ROOT}/fresh.json")
run_configure("${fresh_output}" "" TRUE)
assert_contains("${fresh_output}" "\"query_word_match\"[ \t]*:[ \t]*\"all\"" "fresh configure")

# Legacy extensions are split/deduplicated, true maps to all, and unknown
# config/top-level values survive the template merge.
set(legacy_true "${TEST_ROOT}/legacy-true.json")
file(WRITE "${legacy_true}" [=[{
  "config": {
    "extensions": ["txt", "", "TXT", ".atl"],
    "exact_search": true,
    "future_field": 42
  },
  "future_section": {"keep": true}
}]=])
set(legacy_true_output "${TEST_ROOT}/legacy-true-output.json")
run_configure("${legacy_true_output}" "${legacy_true}" TRUE)
assert_contains("${legacy_true_output}" "\"query_word_match\"[ \t]*:[ \t]*\"all\"" "legacy true")
assert_contains("${legacy_true_output}" "\"include_extensionless_files\"[ \t]*:[ \t]*true" "legacy extensionless")
assert_contains("${legacy_true_output}" "\"future_field\"[ \t]*:[ \t]*42" "unknown config")
assert_contains("${legacy_true_output}" "\"future_section\"" "unknown top-level")
assert_not_contains("${legacy_true_output}" "\"extensions\"" "legacy output")
assert_not_contains("${legacy_true_output}" "\"exact_search\"" "legacy output")

set(legacy_false "${TEST_ROOT}/legacy-false.json")
file(WRITE "${legacy_false}" [=[{
  "config": {"extensions": ["txt"], "exact_search": false}
}]=])
set(legacy_false_output "${TEST_ROOT}/legacy-false-output.json")
run_configure("${legacy_false_output}" "${legacy_false}" TRUE)
assert_contains("${legacy_false_output}" "\"query_word_match\"[ \t]*:[ \t]*\"any\"" "legacy false")

# An installed file without either query field keeps the historical any mode
# instead of inheriting the fresh template's all mode.
set(missing_query "${TEST_ROOT}/missing-query.json")
file(WRITE "${missing_query}" [=[{
  "config": {"extensions": ["txt"], "old_probe": "keep"}
}]=])
set(missing_query_output "${TEST_ROOT}/missing-query-output.json")
run_configure("${missing_query_output}" "${missing_query}" TRUE)
assert_contains("${missing_query_output}" "\"query_word_match\"[ \t]*:[ \t]*\"any\"" "old missing query")

# Canonical values win field-by-field over compatibility aliases.
set(conflict "${TEST_ROOT}/conflict.json")
file(WRITE "${conflict}" [=[{
  "config": {
    "indexed_extensions": ["shp"],
    "include_extensionless_files": false,
    "extensions": ["txt", ""],
    "query_word_match": "any",
    "exact_search": true
  }
}]=])
set(conflict_output "${TEST_ROOT}/conflict-output.json")
run_configure("${conflict_output}" "${conflict}" TRUE)
assert_contains("${conflict_output}" "\"shp\"" "canonical extension wins")
assert_not_contains("${conflict_output}" "\"txt\"" "legacy extension ignored")
assert_contains("${conflict_output}" "\"include_extensionless_files\"[ \t]*:[ \t]*false" "canonical extensionless wins")
assert_contains("${conflict_output}" "\"query_word_match\"[ \t]*:[ \t]*\"any\"" "canonical query wins")

# Invalid canonical values do not fall back to valid aliases.
set(invalid_conflict "${TEST_ROOT}/invalid-conflict.json")
file(WRITE "${invalid_conflict}" [=[{
  "config": {
    "indexed_extensions": [".txt"],
    "include_extensionless_files": false,
    "extensions": ["txt"],
    "query_word_match": "invalid",
    "exact_search": true
  }
}]=])
run_configure("${TEST_ROOT}/invalid-output.json" "${invalid_conflict}" FALSE)

set(wrong_extensionless_type "${TEST_ROOT}/wrong-extensionless-type.json")
file(WRITE "${wrong_extensionless_type}" [=[{
  "config": {
    "indexed_extensions": ["txt"],
    "include_extensionless_files": "yes",
    "query_word_match": "all"
  }
}]=])
run_configure(
    "${TEST_ROOT}/wrong-extensionless-output.json"
    "${wrong_extensionless_type}"
    FALSE)

# Empty canonical array is valid only when extensionless files are enabled.
string(REPLACE
    "\"indexed_extensions\": [\n      \"txt\",\n      \"atl\",\n      \"shp\"\n    ]"
    "\"indexed_extensions\": []"
    extensionless_only "${template_json}")
set(extensionless_only_path "${TEST_ROOT}/extensionless-only.json")
file(WRITE "${extensionless_only_path}" "${extensionless_only}")
execute_process(
    COMMAND "${CONFIG_EXE}" validate --settings "${extensionless_only_path}"
    RESULT_VARIABLE extensionless_rc)
if(NOT extensionless_rc EQUAL 0)
    message(FATAL_ERROR "empty indexed_extensions + true should validate")
endif()
string(REPLACE
    "\"include_extensionless_files\": true"
    "\"include_extensionless_files\": false"
    no_types "${extensionless_only}")
set(no_types_path "${TEST_ROOT}/no-types.json")
file(WRITE "${no_types_path}" "${no_types}")
execute_process(
    COMMAND "${CONFIG_EXE}" validate --settings "${no_types_path}"
    RESULT_VARIABLE no_types_rc)
if(no_types_rc EQUAL 0)
    message(FATAL_ERROR "empty indexed_extensions + false should fail")
endif()

# Re-importing canonical output is idempotent as JSON.
set(repeated_output "${TEST_ROOT}/repeated.json")
run_configure("${repeated_output}" "${legacy_true_output}" TRUE)
execute_process(
    COMMAND "${CONFIG_EXE}" compare-json
        --left "${legacy_true_output}" --right "${repeated_output}"
    RESULT_VARIABLE compare_rc)
if(NOT compare_rc EQUAL 0)
    message(FATAL_ERROR "repeated canonical configure is not idempotent")
endif()
