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

# --- Helper: run inspect and capture port= line ---
macro(assert_inspect_port label settings_file expected_port)
    execute_process(
        COMMAND "${CONFIG_EXE}" inspect --settings "${settings_file}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE  _err
        TIMEOUT 15)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "inspect failed for '${label}' (rc=${_rc})\nstdout: ${_out}\nstderr: ${_err}")
    endif()
    string(REGEX MATCH "port=([0-9]+)" _m "${_out}")
    if(NOT CMAKE_MATCH_1 STREQUAL "${expected_port}")
        message(FATAL_ERROR
            "inspect port mismatch for '${label}': expected ${expected_port}, got '${CMAKE_MATCH_1}'\n"
            "stdout: ${_out}")
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
# 6. sqlite_mirror_max_pending_ops = 0 -> PASS (flush by timer only)
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/pending-ops-zero-valid.json")
write_json_with_field("${template_json}" "${f}" "sqlite_mirror_max_pending_ops" "0")
assert_validate("sqlite_mirror_max_pending_ops zero (timer-only)" "${f}" TRUE)

# ------------------------------------------------------------------
# 7. sqlite_load_threads as string -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-load-threads-string.json")
write_json_with_field("${template_json}" "${f}" "sqlite_load_threads" "\"auto\"")
assert_validate("sqlite_load_threads as string" "${f}" FALSE)

# ------------------------------------------------------------------
# 8. sqlite_load_threads = 0 -> FAIL (must be >= 1)
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-load-threads-zero.json")
write_json_with_field("${template_json}" "${f}" "sqlite_load_threads" "0")
assert_validate("sqlite_load_threads zero" "${f}" FALSE)

# ------------------------------------------------------------------
# 9. max_parallel_readers as string -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-readers-string.json")
write_json_with_field("${template_json}" "${f}" "max_parallel_readers" "\"many\"")
assert_validate("max_parallel_readers as string" "${f}" FALSE)

# ------------------------------------------------------------------
# 10. max_parallel_readers = 0 -> PASS (0 = no limit, valid sentinel)
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/readers-zero-valid.json")
write_json_with_field("${template_json}" "${f}" "max_parallel_readers" "0")
assert_validate("max_parallel_readers zero (no limit)" "${f}" TRUE)

# ------------------------------------------------------------------
# 11. max_response as string -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-max-response-string.json")
write_json_with_field("${template_json}" "${f}" "max_response" "\"all\"")
assert_validate("max_response as string" "${f}" FALSE)

# ------------------------------------------------------------------
# 12. max_response = 0 -> PASS (returns 0 results; valid operational value)
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/max-response-zero-valid.json")
write_json_with_field("${template_json}" "${f}" "max_response" "0")
assert_validate("max_response zero (valid, returns 0 results)" "${f}" TRUE)

# ------------------------------------------------------------------
# 13. ind_time = 0 -> FAIL (must be >= 1)
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-ind-time-zero.json")
write_json_with_field("${template_json}" "${f}" "ind_time" "0")
assert_validate("ind_time zero" "${f}" FALSE)

# ------------------------------------------------------------------
# 14. compact_threshold_percent as string -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-compact-string.json")
write_json_with_field("${template_json}" "${f}" "compact_threshold_percent" "\"high\"")
assert_validate("compact_threshold_percent as string" "${f}" FALSE)

# ------------------------------------------------------------------
# 15. compact_threshold_percent = 150 -> FAIL (must be 0..100)
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-compact-over.json")
write_json_with_field("${template_json}" "${f}" "compact_threshold_percent" "150")
assert_validate("compact_threshold_percent over 100" "${f}" FALSE)

# ------------------------------------------------------------------
# 16. Missing dirs without --check-dirs -> PASS
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
# 17. Valid positive max_parallel_readers -> PASS
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/valid-readers-positive.json")
write_json_with_field("${template_json}" "${f}" "max_parallel_readers" "4")
assert_validate("max_parallel_readers positive" "${f}" TRUE)

# ------------------------------------------------------------------
# 18. exact_search as string -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-exact-search-string.json")
write_json_with_field("${template_json}" "${f}" "exact_search" "\"yes\"")
assert_validate("exact_search as string" "${f}" FALSE)

# ------------------------------------------------------------------
# 19. hide_mode as integer -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-hide-mode-int.json")
write_json_with_field("${template_json}" "${f}" "hide_mode" "1")
assert_validate("hide_mode as integer" "${f}" FALSE)

# ------------------------------------------------------------------
# 20. text_request as string -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-text-request-string.json")
write_json_with_field("${template_json}" "${f}" "text_request" "\"true\"")
assert_validate("text_request as string" "${f}" FALSE)

# ------------------------------------------------------------------
# 21. save_dictionary_to_file as integer -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-save-dict-int.json")
write_json_with_field("${template_json}" "${f}" "save_dictionary_to_file" "0")
assert_validate("save_dictionary_to_file as integer" "${f}" FALSE)

# ------------------------------------------------------------------
# 21b. representable INT_MAX boundaries for int destinations
# ------------------------------------------------------------------
math(EXPR INT_MAX "2147483647")
math(EXPR INT_MAX_PLUS1 "${INT_MAX}+1")

# max_response: 0..INT_MAX allowed
set(f "${TEST_ROOT}/max-response-intmax-valid.json")
write_json_with_field("${template_json}" "${f}" "max_response" "${INT_MAX}")
assert_validate("max_response INT_MAX" "${f}" TRUE)

set(f "${TEST_ROOT}/max-response-intmax-over.json")
write_json_with_field("${template_json}" "${f}" "max_response" "${INT_MAX_PLUS1}")
assert_validate("max_response INT_MAX+1" "${f}" FALSE)

# max_parallel_readers: 0..INT_MAX allowed (0 sentinel is already covered)
set(f "${TEST_ROOT}/max-parallel-readers-intmax-valid.json")
write_json_with_field("${template_json}" "${f}" "max_parallel_readers" "${INT_MAX}")
assert_validate("max_parallel_readers INT_MAX" "${f}" TRUE)

set(f "${TEST_ROOT}/max-parallel-readers-intmax-over.json")
write_json_with_field("${template_json}" "${f}" "max_parallel_readers" "${INT_MAX_PLUS1}")
assert_validate("max_parallel_readers INT_MAX+1" "${f}" FALSE)

# sqlite_mirror_max_pending_ops: 0..INT_MAX allowed (0 sentinel is already covered)
set(f "${TEST_ROOT}/pending-ops-intmax-valid.json")
write_json_with_field("${template_json}" "${f}" "sqlite_mirror_max_pending_ops" "${INT_MAX}")
assert_validate("sqlite_mirror_max_pending_ops INT_MAX" "${f}" TRUE)

set(f "${TEST_ROOT}/pending-ops-intmax-over.json")
write_json_with_field("${template_json}" "${f}" "sqlite_mirror_max_pending_ops" "${INT_MAX_PLUS1}")
assert_validate("sqlite_mirror_max_pending_ops INT_MAX+1" "${f}" FALSE)

# sqlite_load_threads: 1..INT_MAX allowed
set(f "${TEST_ROOT}/sqlite-load-threads-intmax-valid.json")
write_json_with_field("${template_json}" "${f}" "sqlite_load_threads" "${INT_MAX}")
assert_validate("sqlite_load_threads INT_MAX" "${f}" TRUE)

set(f "${TEST_ROOT}/sqlite-load-threads-intmax-over.json")
write_json_with_field("${template_json}" "${f}" "sqlite_load_threads" "${INT_MAX_PLUS1}")
assert_validate("sqlite_load_threads INT_MAX+1" "${f}" FALSE)

# ------------------------------------------------------------------
# 21c. ind_time representable-range checks depend on architecture
# ------------------------------------------------------------------
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    # x86: ind_time must not exceed SIZE_MAX (2^32-1); overflow fails.
    set(f "${TEST_ROOT}/ind-time-over-size_t-x86.json")
    write_json_with_field("${template_json}" "${f}" "ind_time" "4294967296")
    assert_validate("ind_time over SIZE_MAX (x86)" "${f}" FALSE)
else()
    # x64: value within size_t range must PASS.
    set(f "${TEST_ROOT}/ind-time-within-size_t-x64.json")
    write_json_with_field("${template_json}" "${f}" "ind_time" "4294967295")
    assert_validate("ind_time within size_t (x64/other)" "${f}" TRUE)
endif()

# ------------------------------------------------------------------
# Build a compact single-line baseline for array element tests.
# This avoids multi-line regex issues with the template.
# ------------------------------------------------------------------
set(compact_base [=[{
  "config": {
    "Name": "TestEngine",
    "asio_port": 15006,
    "batch_indexer_threads": 0,
    "batch_queue_memory_mb": 256,
    "batch_reader_threads": 1,
    "compact_threshold_percent": 5.0,
    "dir": "D:\\",
    "dirs": ["D:\\TEST"],
    "exact_search": true,
    "exclude_dirs": [],
    "extensions": ["txt"],
    "enable_prm_short_content_autodetect": true,
    "file_indexing_timeout_sec": 120,
    "full_index_strategy": "batch",
    "document_catalog_storage": "memory",
    "hide_mode": false,
    "ind_time": 500,
    "max_parallel_readers": 0,
    "max_response": 50000,
    "prd_base_dir": "D:\\BASES_PRD",
    "prm_base_dir": "D:\\BASES",
    "tlg_send_root": "D:\\",
    "razn_output_dir": "D:\\RAZN",
    "opis_base_dir": "D:\\OPIS",
    "f12_base_dir": "D:\\F12",
    "save_dictionary_to_file": true,
    "scan_on_startup": true,
    "sqlite_load_threads": 4,
    "sqlite_mirror_flush_interval_sec": 2.0,
    "sqlite_mirror_max_pending_ops": 500,
    "sqlite_precount_postings": false,
    "text_request": true,
    "thread_count": 4,
    "year": "2026"
  }
}]=])
set(compact_base_file "${TEST_ROOT}/compact-base.json")
file(WRITE "${compact_base_file}" "${compact_base}")
assert_validate("compact baseline" "${compact_base_file}" TRUE)

# ------------------------------------------------------------------
# 22. dirs containing a number element -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-dirs-element.json")
write_json_with_field("${compact_base}" "${f}" "dirs" "[\"D:\\\\TEST\", 42]")
assert_validate("dirs element is number" "${f}" FALSE)

# ------------------------------------------------------------------
# 23. extensions containing a number element -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-extensions-element.json")
write_json_with_field("${compact_base}" "${f}" "extensions" "[\"txt\", 99]")
assert_validate("extensions element is number" "${f}" FALSE)

# ------------------------------------------------------------------
# 24. exclude_dirs as a plain string (not array) -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-exclude-dirs-string.json")
write_json_with_field("${compact_base}" "${f}" "exclude_dirs" "\"oops\"")
assert_validate("exclude_dirs as string not array" "${f}" FALSE)

# ------------------------------------------------------------------
# 25. exclude_dirs containing a number element -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-exclude-dirs-element.json")
write_json_with_field("${compact_base}" "${f}" "exclude_dirs" "[\"C:/ok\", 123]")
assert_validate("exclude_dirs element is number" "${f}" FALSE)

# ------------------------------------------------------------------
# 26. Files (top-level) as plain string -> FAIL
# Build a JSON with "Files" as a string at root level.
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-files-string.json")
set(bad_files_str [=[{"Files": "oops", "config": {
    "Name": "TestEngine", "asio_port": 15006,
    "batch_indexer_threads": 0, "batch_queue_memory_mb": 256,
    "batch_reader_threads": 1, "compact_threshold_percent": 5.0,
    "dir": "D:\\", "dirs": ["D:\\TEST"], "exact_search": true,
    "exclude_dirs": [], "extensions": ["txt"],
    "enable_prm_short_content_autodetect": true,
    "file_indexing_timeout_sec": 120, "full_index_strategy": "batch",
    "document_catalog_storage": "memory", "hide_mode": false,
    "ind_time": 500, "max_parallel_readers": 0, "max_response": 50000,
    "prd_base_dir": "D:\\BASES_PRD", "prm_base_dir": "D:\\BASES",
    "tlg_send_root": "D:\\", "razn_output_dir": "D:\\RAZN",
    "opis_base_dir": "D:\\OPIS", "f12_base_dir": "D:\\F12",
    "save_dictionary_to_file": true, "scan_on_startup": true,
    "sqlite_load_threads": 4, "sqlite_mirror_flush_interval_sec": 2.0,
    "sqlite_mirror_max_pending_ops": 500, "sqlite_precount_postings": false,
    "text_request": true, "thread_count": 4, "year": "2026"
  }}]=])
file(WRITE "${f}" "${bad_files_str}")
assert_validate("Files as string not array" "${f}" FALSE)

# ------------------------------------------------------------------
# 26b. Files (top-level) containing a number element -> FAIL
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/bad-files-element.json")
set(bad_files_el_str [=[{"Files": ["ok.txt", 42], "config": {
    "Name": "TestEngine", "asio_port": 15006,
    "batch_indexer_threads": 0, "batch_queue_memory_mb": 256,
    "batch_reader_threads": 1, "compact_threshold_percent": 5.0,
    "dir": "D:\\", "dirs": ["D:\\TEST"], "exact_search": true,
    "exclude_dirs": [], "extensions": ["txt"],
    "enable_prm_short_content_autodetect": true,
    "file_indexing_timeout_sec": 120, "full_index_strategy": "batch",
    "document_catalog_storage": "memory", "hide_mode": false,
    "ind_time": 500, "max_parallel_readers": 0, "max_response": 50000,
    "prd_base_dir": "D:\\BASES_PRD", "prm_base_dir": "D:\\BASES",
    "tlg_send_root": "D:\\", "razn_output_dir": "D:\\RAZN",
    "opis_base_dir": "D:\\OPIS", "f12_base_dir": "D:\\F12",
    "save_dictionary_to_file": true, "scan_on_startup": true,
    "sqlite_load_threads": 4, "sqlite_mirror_flush_interval_sec": 2.0,
    "sqlite_mirror_max_pending_ops": 500, "sqlite_precount_postings": false,
    "text_request": true, "thread_count": 4, "year": "2026"
  }}]=])
file(WRITE "${f}" "${bad_files_el_str}")
assert_validate("Files element is number" "${f}" FALSE)

# ------------------------------------------------------------------
# 27. Port precedence: legacy port only (no asio_port) -> PASS
#     inspect must return the same port value (15001) as runtime uses.
#     Use compact_base which has asio_port; replace with port only.
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/port-only.json")
write_json_with_field("${compact_base}" "${f}" "port" "15001")
# Also ensure asio_port is removed by replacing it with something harmless;
# write_json_with_field will have set asio_port -> if it existed it remains.
# Re-read and strip asio_port line:
file(READ "${f}" _port_only_content)
string(REGEX REPLACE "\"asio_port\"[ \t]*:[ \t]*[^\n,}]+,?" "" _port_only_content "${_port_only_content}")
file(WRITE "${f}" "${_port_only_content}")
assert_validate("legacy port only" "${f}" TRUE)
assert_inspect_port("legacy port only" "${f}" "15001")

# ------------------------------------------------------------------
# 28. asio_port only (no port field) -> PASS; inspect returns asio_port value.
#     compact_base already has asio_port=15006 — just validate and inspect.
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/asio-port-only.json")
file(WRITE "${f}" "${compact_base}")
assert_validate("asio_port only" "${f}" TRUE)
assert_inspect_port("asio_port only" "${f}" "15006")

# ------------------------------------------------------------------
# 29. Both port + asio_port -> PASS; inspect returns port value (runtime precedence).
#     Add port=15003 alongside existing asio_port=15006.
# ------------------------------------------------------------------
set(f "${TEST_ROOT}/both-ports.json")
write_json_with_field("${compact_base}" "${f}" "port" "15003")
assert_validate("both port and asio_port" "${f}" TRUE)
assert_inspect_port("both port and asio_port (runtime precedence: port wins)" "${f}" "15003")

message(STATUS "SearchEngineConfigValidateSVC001Fields: all checks passed")
