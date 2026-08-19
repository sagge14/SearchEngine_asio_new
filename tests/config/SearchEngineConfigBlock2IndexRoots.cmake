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

message(STATUS "SearchEngineConfigBlock2IndexRoots: all checks passed")
