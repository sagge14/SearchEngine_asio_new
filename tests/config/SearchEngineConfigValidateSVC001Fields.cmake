cmake_policy(SET CMP0012 NEW)
if(NOT DEFINED CONFIG_EXE OR NOT DEFINED TEMPLATE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "CONFIG_EXE, TEMPLATE and TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(READ "${TEMPLATE}" template_json)

# --- Helper: run validate and assert expected result ---
macro(assert_validate label settings_file expect_success)
    execute_process(
        COMMAND "${CONFIG_EXE}" validate --settings "${settings_file}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE  _err
        TIMEOUT 15)
    if("${expect_success}" STREQUAL "TRUE")
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR
                "validate should PASS for '${label}' but failed (rc=${_rc})\n"
                "stdout: ${_out}\nstderr: ${_err}")
        endif()
    else()
        if(_rc EQUAL 0)
            message(FATAL_ERROR
                "validate should FAIL for '${label}' but succeeded\n"
                "stdout: ${_out}\nstderr: ${_err}")
        endif()
    endif()
endmacro()

# --- Helper: replace OR insert a top-level config field.
# key_name: the JSON key (no quotes, no colon)
# new_value: the new JSON value (e.g. "\"yes\"" or "0" or "true")
# If the key already exists in the JSON, its value is replaced.
# Otherwise the key=value pair is appended inside the config object.
function(write_json_with_field input_json output_file key_name new_value)
    # Try to replace existing value first (handles number, bool, string values).
    string(REGEX REPLACE
        "(\"${key_name}\"[ \t]*:[ \t]*)([^\n,}]+)"
        "\\1${new_value}"
        modified
        "${input_json}")
    if("${modified}" STREQUAL "${input_json}")
        # Field not found; append before closing brace of config object.
        string(REGEX REPLACE
            "([ \t]*\"enable_prm_short_content_autodetect\"[ \t]*:[ \t]*[^\n,}]+)"
            "\\1,\n    \"${key_name}\": ${new_value}"
            modified
            "${input_json}")
    endif()
    file(WRITE "${output_file}" "${modified}")
endfunction()

# ------------------------------------------------------------------
# 1. Template itself must PASS (baseline sanity)
# ------------------------------------------------------------------
set(baseline "${TEST_ROOT}/baseline.json")
file(WRITE "${baseline}" "${template_json}")
assert_validate("baseline template" "${baseline}" TRUE)

# ------------------------------------------------------------------
# 2. sqlite_precount_postings as string -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-precount-string.json")
write_json_with_field("${template_json}" "${f}" "sqlite_precount_postings" "\"yes\"")
assert_validate("sqlite_precount_postings as string" "${f}" FALSE)

# ------------------------------------------------------------------
# 3. sqlite_mirror_flush_interval_sec as string -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-flush-string.json")
write_json_with_field("${template_json}" "${f}" "sqlite_mirror_flush_interval_sec" "\"fast\"")
assert_validate("sqlite_mirror_flush_interval_sec as string" "${f}" FALSE)

# ------------------------------------------------------------------
# 4. sqlite_mirror_flush_interval_sec = 0 -> FAIL (must be > 0)
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-flush-zero.json")
write_json_with_field("${template_json}" "${f}" "sqlite_mirror_flush_interval_sec" "0")
assert_validate("sqlite_mirror_flush_interval_sec zero" "${f}" FALSE)

# ------------------------------------------------------------------
# 5. sqlite_mirror_max_pending_ops as string -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-pending-string.json")
write_json_with_field("${template_json}" "${f}" "sqlite_mirror_max_pending_ops" "\"many\"")
assert_validate("sqlite_mirror_max_pending_ops as string" "${f}" FALSE)

# ------------------------------------------------------------------
# 6. sqlite_load_threads as string -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-load-threads-string.json")
write_json_with_field("${template_json}" "${f}" "sqlite_load_threads" "\"auto\"")
assert_validate("sqlite_load_threads as string" "${f}" FALSE)

# ------------------------------------------------------------------
# 7. sqlite_load_threads = 0 -> FAIL (must be 1..64)
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-load-threads-zero.json")
write_json_with_field("${template_json}" "${f}" "sqlite_load_threads" "0")
assert_validate("sqlite_load_threads zero" "${f}" FALSE)

# ------------------------------------------------------------------
# 8. max_parallel_readers as string -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-readers-string.json")
write_json_with_field("${template_json}" "${f}" "max_parallel_readers" "\"many\"")
assert_validate("max_parallel_readers as string" "${f}" FALSE)

# ------------------------------------------------------------------
# 9. max_parallel_readers = 0 -> PASS (0 = no limit, valid sentinel)
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/readers-zero-valid.json")
write_json_with_field("${template_json}" "${f}" "max_parallel_readers" "0")
assert_validate("max_parallel_readers zero (no limit)" "${f}" TRUE)

# ------------------------------------------------------------------
# 10. max_response as string -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-max-response-string.json")
write_json_with_field("${template_json}" "${f}" "max_response" "\"all\"")
assert_validate("max_response as string" "${f}" FALSE)

# ------------------------------------------------------------------
# 11. max_response = 0 -> FAIL (must be >= 1)
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-max-response-zero.json")
write_json_with_field("${template_json}" "${f}" "max_response" "0")
assert_validate("max_response zero" "${f}" FALSE)

# ------------------------------------------------------------------
# 12. ind_time = 0 -> FAIL (must be >= 1)
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-ind-time-zero.json")
write_json_with_field("${template_json}" "${f}" "ind_time" "0")
assert_validate("ind_time zero" "${f}" FALSE)

# ------------------------------------------------------------------
# 13. compact_threshold_percent as string -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-compact-string.json")
write_json_with_field("${template_json}" "${f}" "compact_threshold_percent" "\"high\"")
assert_validate("compact_threshold_percent as string" "${f}" FALSE)

# ------------------------------------------------------------------
# 14. compact_threshold_percent = 150 -> FAIL (must be 0..100)
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-compact-over.json")
write_json_with_field("${template_json}" "${f}" "compact_threshold_percent" "150")
assert_validate("compact_threshold_percent over 100" "${f}" FALSE)

# ------------------------------------------------------------------
# 15. Missing dirs without --check-dirs -> PASS
#     (dirs existence is only checked when --check-dirs is provided)
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/missing-dirs.json")
string(REGEX REPLACE
    "(\"dirs\"[ \t]*:[ \t]*\\[)[^\\]]*\\]"
    "\\1\"/nonexistent/path/for/test\"]"
    no_dirs_json
    "${template_json}")
file(WRITE "${f}" "${no_dirs_json}")
assert_validate("missing dirs without --check-dirs" "${f}" TRUE)

# ------------------------------------------------------------------
# 16. Valid positive max_parallel_readers -> PASS
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/valid-readers-positive.json")
write_json_with_field("${template_json}" "${f}" "max_parallel_readers" "4")
assert_validate("max_parallel_readers positive" "${f}" TRUE)

message(STATUS "SearchEngineConfigValidateSVC001Fields: all checks passed")
