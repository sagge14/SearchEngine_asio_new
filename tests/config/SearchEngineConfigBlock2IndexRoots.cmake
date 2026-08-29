if(NOT DEFINED CONFIG_EXE OR NOT DEFINED TEMPLATE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "CONFIG_EXE, TEMPLATE and TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(READ "${TEMPLATE}" template_json)

function(run_configure output_path import_settings)
    set(configure_args
        configure
        --template "${TEMPLATE}"
        --output "${output_path}"
        --port 15001
        --year 2026
        --threads 2
        --file-timeout 120
        --prm-autodetect 1
        --quiet)
    if(import_settings)
        list(APPEND configure_args --import-settings "${import_settings}")
    endif()
    execute_process(
        COMMAND "${CONFIG_EXE}" ${configure_args}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        TIMEOUT 15)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "configure failed (rc=${_rc})\nstdout: ${_out}\nstderr: ${_err}")
    endif()
endfunction()

function(assert_json_key_absent file label key)
    file(READ "${file}" content)
    if(content MATCHES "\"${key}\"")
        message(FATAL_ERROR "${label}: retired key '${key}' still present")
    endif()
endfunction()

function(assert_json_key_present file label key)
    file(READ "${file}" content)
    if(NOT content MATCHES "\"${key}\"")
        message(FATAL_ERROR "${label}: expected key '${key}' missing")
    endif()
endfunction()

# ------------------------------------------------------------------
# dirs -> index_roots migration preserves user value over template default
# ------------------------------------------------------------------
set(old_dirs_only "${TEST_ROOT}/old-dirs-only.json")
file(WRITE "${old_dirs_only}" [=[{
  "config": {
    "year": "2026",
    "dirs": ["E:\\CUSTOM"],
    "extensions": ["txt"],
    "prm_base_dir": "",
    "prd_base_dir": "",
    "tlg_send_root": "D:\\",
    "razn_output_dir": "D:\\OPIS_ADMIN\\РАЗНОСКА_ДЛЯ_ПРОСТАВЛЕНИЯ",
    "opis_base_dir": "D:\\OPIS_ADMIN",
    "f12_base_dir": "D:\\F12",
    "thread_count": 4,
    "file_indexing_timeout_sec": 120
  }
}]=])

set(configured_dirs "${TEST_ROOT}/configured-dirs-only.json")
run_configure("${configured_dirs}" "${old_dirs_only}")

assert_json_key_present("${configured_dirs}" "configured output" "index_roots")
assert_json_key_absent("${configured_dirs}" "configured output" "dirs")
file(READ "${configured_dirs}" configured_dirs_content)
if(NOT configured_dirs_content MATCHES "CUSTOM")
    message(FATAL_ERROR "legacy dirs value was not migrated to index_roots")
endif()
if(configured_dirs_content MATCHES "\"dirs\"")
    message(FATAL_ERROR "legacy dirs key was not removed")
endif()

# ------------------------------------------------------------------
# canonical index_roots wins over legacy dirs
# ------------------------------------------------------------------
set(conflict_settings "${TEST_ROOT}/conflict-index-roots.json")
file(WRITE "${conflict_settings}" [=[{
  "config": {
    "year": "2026",
    "dirs": ["D:\\OLD"],
    "index_roots": ["D:\\NEW"],
    "extensions": ["txt"],
    "prm_base_dir": "",
    "prd_base_dir": "",
    "tlg_send_root": "D:\\",
    "razn_output_dir": "D:\\OPIS_ADMIN\\РАЗНОСКА_ДЛЯ_ПРОСТАВЛЕНИЯ",
    "opis_base_dir": "D:\\OPIS_ADMIN",
    "f12_base_dir": "D:\\F12",
    "thread_count": 4,
    "file_indexing_timeout_sec": 120
  }
}]=])

set(conflict_output "${TEST_ROOT}/configured-conflict-index-roots.json")
run_configure("${conflict_output}" "${conflict_settings}")

file(READ "${conflict_output}" conflict_content)
if(NOT conflict_content MATCHES "NEW")
    message(FATAL_ERROR "index_roots did not win over legacy dirs")
endif()
if(conflict_content MATCHES "OLD")
    message(FATAL_ERROR "legacy dirs value leaked into configured output")
endif()
assert_json_key_absent("${conflict_output}" "conflict output" "dirs")

# ------------------------------------------------------------------
# exclude_dirs -> excluded_subtrees migration
# ------------------------------------------------------------------
set(old_exclude "${TEST_ROOT}/old-exclude-dirs.json")
file(WRITE "${old_exclude}" [=[{
  "config": {
    "year": "2026",
    "dirs": ["D:\\DATA"],
    "exclude_dirs": ["D:\\DATA\\TEMP"],
    "extensions": ["txt"],
    "prm_base_dir": "",
    "prd_base_dir": "",
    "tlg_send_root": "D:\\",
    "razn_output_dir": "D:\\OPIS_ADMIN\\РАЗНОСКА_ДЛЯ_ПРОСТАВЛЕНИЯ",
    "opis_base_dir": "D:\\OPIS_ADMIN",
    "f12_base_dir": "D:\\F12",
    "thread_count": 4,
    "file_indexing_timeout_sec": 120
  }
}]=])

set(configured_exclude "${TEST_ROOT}/configured-exclude-dirs.json")
run_configure("${configured_exclude}" "${old_exclude}")

assert_json_key_present("${configured_exclude}" "configured output" "excluded_subtrees")
assert_json_key_absent("${configured_exclude}" "configured output" "exclude_dirs")
file(READ "${configured_exclude}" configured_exclude_content)
if(NOT configured_exclude_content MATCHES "TEMP")
    message(FATAL_ERROR "exclude_dirs value was not migrated to excluded_subtrees")
endif()

execute_process(
    COMMAND "${CONFIG_EXE}" validate --settings "${configured_dirs}"
    RESULT_VARIABLE validate_rc
    OUTPUT_VARIABLE validate_out
    ERROR_VARIABLE validate_err
    TIMEOUT 15)
if(NOT validate_rc EQUAL 0 OR NOT validate_out MATCHES "settings_valid=1")
    message(FATAL_ERROR
        "configured output failed validate: rc=${validate_rc} "
        "out=${validate_out} err=${validate_err}")
endif()

function(write_and_validate_expect file_name json_text expect_success label)
    set(_path "${TEST_ROOT}/${file_name}")
    file(WRITE "${_path}" "${json_text}")
    execute_process(
        COMMAND "${CONFIG_EXE}" validate --settings "${_path}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
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
endfunction()

set(block2_base [=[
{
  "config": {
    "asio_port": 15006,
    "batch_indexer_threads": 0,
    "batch_queue_memory_mb": 256,
    "batch_reader_threads": 1,
    "compact_threshold_percent": 5.0,
    "index_roots": ["D:\\TEST"],
    "exact_search": true,
    "excluded_subtrees": [],
    "extensions": ["txt"],
    "enable_prm_short_content_autodetect": true,
    "file_indexing_timeout_sec": 120,
    "full_index_strategy": "batch",
    "document_catalog_storage": "memory",
    "hide_console_window": false,
    "ind_time": 500,
    "max_parallel_readers": 0,
    "max_response": 50000,
    "prd_base_dir": "D:\\BASES_PRD",
    "prm_base_dir": "D:\\BASES",
    "tlg_send_root": "D:\\",
    "razn_output_dir": "D:\\RAZN",
    "opis_base_dir": "D:\\OPIS",
    "f12_base_dir": "D:\\F12",
    "scan_on_startup": true,
    "sqlite_load_threads": 4,
    "sqlite_mirror_flush_interval_sec": 2.0,
    "sqlite_mirror_max_pending_ops": 500,
    "sqlite_precount_postings": false,
    "thread_count": 4,
    "year": "2026"
  }
}
]=])

write_and_validate_expect(
    "empty-index-roots-with-dirs.json"
    "{
  \"config\": {
    \"asio_port\": 15006,
    \"batch_indexer_threads\": 0,
    \"batch_queue_memory_mb\": 256,
    \"batch_reader_threads\": 1,
    \"compact_threshold_percent\": 5.0,
    \"index_roots\": [],
    \"dirs\": [\"D:\\\\OLD\"],
    \"exact_search\": true,
    \"excluded_subtrees\": [],
    \"extensions\": [\"txt\"],
    \"enable_prm_short_content_autodetect\": true,
    \"file_indexing_timeout_sec\": 120,
    \"full_index_strategy\": \"batch\",
    \"document_catalog_storage\": \"memory\",
    \"hide_console_window\": false,
    \"ind_time\": 500,
    \"max_parallel_readers\": 0,
    \"max_response\": 50000,
    \"prd_base_dir\": \"D:\\\\BASES_PRD\",
    \"prm_base_dir\": \"D:\\\\BASES\",
    \"tlg_send_root\": \"D:\\\\\",
    \"razn_output_dir\": \"D:\\\\RAZN\",
    \"opis_base_dir\": \"D:\\\\OPIS\",
    \"f12_base_dir\": \"D:\\\\F12\",
    \"scan_on_startup\": true,
    \"sqlite_load_threads\": 4,
    \"sqlite_mirror_flush_interval_sec\": 2.0,
    \"sqlite_mirror_max_pending_ops\": 500,
    \"sqlite_precount_postings\": false,
    \"thread_count\": 4,
    \"year\": \"2026\"
  }
}"
    FALSE
    "empty canonical index_roots with valid dirs")

write_and_validate_expect(
    "wrong-type-index-roots-with-dirs.json"
    "{
  \"config\": {
    \"asio_port\": 15006,
    \"batch_indexer_threads\": 0,
    \"batch_queue_memory_mb\": 256,
    \"batch_reader_threads\": 1,
    \"compact_threshold_percent\": 5.0,
    \"index_roots\": \"bad\",
    \"dirs\": [\"D:\\\\OLD\"],
    \"exact_search\": true,
    \"excluded_subtrees\": [],
    \"extensions\": [\"txt\"],
    \"enable_prm_short_content_autodetect\": true,
    \"file_indexing_timeout_sec\": 120,
    \"full_index_strategy\": \"batch\",
    \"document_catalog_storage\": \"memory\",
    \"hide_console_window\": false,
    \"ind_time\": 500,
    \"max_parallel_readers\": 0,
    \"max_response\": 50000,
    \"prd_base_dir\": \"D:\\\\BASES_PRD\",
    \"prm_base_dir\": \"D:\\\\BASES\",
    \"tlg_send_root\": \"D:\\\\\",
    \"razn_output_dir\": \"D:\\\\RAZN\",
    \"opis_base_dir\": \"D:\\\\OPIS\",
    \"f12_base_dir\": \"D:\\\\F12\",
    \"scan_on_startup\": true,
    \"sqlite_load_threads\": 4,
    \"sqlite_mirror_flush_interval_sec\": 2.0,
    \"sqlite_mirror_max_pending_ops\": 500,
    \"sqlite_precount_postings\": false,
    \"thread_count\": 4,
    \"year\": \"2026\"
  }
}"
    FALSE
    "wrong-type canonical index_roots with valid dirs")

write_and_validate_expect(
    "legacy-dirs-only.json"
    "{
  \"config\": {
    \"asio_port\": 15006,
    \"batch_indexer_threads\": 0,
    \"batch_queue_memory_mb\": 256,
    \"batch_reader_threads\": 1,
    \"compact_threshold_percent\": 5.0,
    \"dirs\": [\"D:\\\\OLD\"],
    \"exact_search\": true,
    \"extensions\": [\"txt\"],
    \"enable_prm_short_content_autodetect\": true,
    \"file_indexing_timeout_sec\": 120,
    \"full_index_strategy\": \"batch\",
    \"document_catalog_storage\": \"memory\",
    \"hide_console_window\": false,
    \"ind_time\": 500,
    \"max_parallel_readers\": 0,
    \"max_response\": 50000,
    \"prd_base_dir\": \"D:\\\\BASES_PRD\",
    \"prm_base_dir\": \"D:\\\\BASES\",
    \"tlg_send_root\": \"D:\\\\\",
    \"razn_output_dir\": \"D:\\\\RAZN\",
    \"opis_base_dir\": \"D:\\\\OPIS\",
    \"f12_base_dir\": \"D:\\\\F12\",
    \"scan_on_startup\": true,
    \"sqlite_load_threads\": 4,
    \"sqlite_mirror_flush_interval_sec\": 2.0,
    \"sqlite_mirror_max_pending_ops\": 500,
    \"sqlite_precount_postings\": false,
    \"thread_count\": 4,
    \"year\": \"2026\"
  }
}"
    TRUE
    "legacy dirs without index_roots")

string(REPLACE
    [=["index_roots": ["D:\\TEST"]]=]
    [=["index_roots": ["\\\\server\\share\\DATA"]]=]
    unc_roots_json
    "${block2_base}")
write_and_validate_expect(
    "unc-index-roots.json" "${unc_roots_json}" TRUE "UNC index_roots")

string(REPLACE
    [=["excluded_subtrees": []]=]
    [=["excluded_subtrees": ["\\\\server\\share\\DATA\\TEMP"]]=]
    unc_excl_json
    "${block2_base}")
write_and_validate_expect(
    "unc-excluded-subtrees.json" "${unc_excl_json}" TRUE "UNC excluded_subtrees")

string(REPLACE
    [=["index_roots": ["D:\\TEST"]]=]
    [=["index_roots": ["relative\\data"]]=]
    rel_roots_json
    "${block2_base}")
write_and_validate_expect(
    "relative-index-roots.json" "${rel_roots_json}" FALSE "relative index_roots")

string(REPLACE
    [=["excluded_subtrees": []]=]
    [=["excluded_subtrees": ["relative\\temp"]]=]
    rel_excl_json
    "${block2_base}")
write_and_validate_expect(
    "relative-excluded-subtrees.json" "${rel_excl_json}" FALSE "relative excluded_subtrees")

string(REPLACE
    [=["excluded_subtrees": []]=]
    [=["excluded_subtrees": [""]]=]
    empty_excl_json
    "${block2_base}")
write_and_validate_expect(
    "empty-string-excluded-subtrees.json" "${empty_excl_json}" FALSE
    "excluded_subtrees empty string")

message(STATUS "SearchEngineConfigBlock2IndexRoots: all checks passed")
