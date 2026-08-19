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
# E. Fresh template has no retired BLOCK-1 fields
# ------------------------------------------------------------------
foreach(retired IN ITEMS Name Version dir text_request save_dictionary_to_file hide_mode)
    assert_json_key_absent("${TEMPLATE}" "release template" "${retired}")
endforeach()
assert_json_key_absent("${TEMPLATE}" "release template top-level" "Files")
assert_json_key_present("${TEMPLATE}" "release template" "hide_console_window")

# ------------------------------------------------------------------
# C/D. Old install explicit update + unknown field preservation
# ------------------------------------------------------------------
set(old_settings "${TEST_ROOT}/old-installed.json")
file(WRITE "${old_settings}" [=[{
  "config": {
    "Name": "Server",
    "Version": "1.1",
    "dir": "D:\\",
    "hide_mode": true,
    "text_request": false,
    "save_dictionary_to_file": false,
    "year": "2025",
    "dirs": ["D:\\KEEP_ME"],
    "extensions": ["txt"],
    "prm_base_dir": "",
    "prd_base_dir": "",
    "tlg_send_root": "D:\\",
    "razn_output_dir": "D:\\OPIS_ADMIN\\РАЗНОСКА_ДЛЯ_ПРОСТАВЛЕНИЯ",
    "opis_base_dir": "D:\\OPIS_ADMIN",
    "f12_base_dir": "D:\\F12",
    "thread_count": 4,
    "file_indexing_timeout_sec": 120,
    "my_future_field": 123
  },
  "Files": ["D:\\a.txt"],
  "custom_section": { "abc": true }
}]=])

set(configured "${TEST_ROOT}/configured-from-old.json")
run_configure("${configured}" "${old_settings}")

foreach(retired IN ITEMS Name Version dir text_request save_dictionary_to_file hide_mode)
    assert_json_key_absent("${configured}" "configured output" "${retired}")
endforeach()
assert_json_key_absent("${configured}" "configured output top-level" "Files")
assert_json_key_present("${configured}" "configured output" "hide_console_window")

file(READ "${configured}" configured_content)
if(NOT configured_content MATCHES "\"hide_console_window\"[ \t]*:[ \t]*true")
    message(FATAL_ERROR "hide_mode=true was not migrated to hide_console_window=true")
endif()
if(NOT configured_content MATCHES "KEEP_ME")
    message(FATAL_ERROR "user dirs entry was lost during configure")
endif()
if(NOT configured_content MATCHES "\"my_future_field\"[ \t]*:[ \t]*123")
    message(FATAL_ERROR "unknown config field was lost during configure")
endif()
if(NOT configured_content MATCHES "\"custom_section\"")
    message(FATAL_ERROR "unknown top-level section was lost during configure")
endif()

# ------------------------------------------------------------------
# Conflict: hide_console_window wins over hide_mode
# ------------------------------------------------------------------
set(conflict_settings "${TEST_ROOT}/conflict-installed.json")
file(WRITE "${conflict_settings}" [=[{
  "config": {
    "hide_console_window": false,
    "hide_mode": true,
    "year": "2026",
    "dirs": ["D:\\TEST"],
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

set(conflict_output "${TEST_ROOT}/configured-conflict.json")
run_configure("${conflict_output}" "${conflict_settings}")

file(READ "${conflict_output}" conflict_content)
if(NOT conflict_content MATCHES "\"hide_console_window\"[ \t]*:[ \t]*false")
    message(FATAL_ERROR "hide_console_window=false did not win over hide_mode=true")
endif()
assert_json_key_absent("${conflict_output}" "conflict output" "hide_mode")

execute_process(
    COMMAND "${CONFIG_EXE}" validate --settings "${configured}"
    RESULT_VARIABLE validate_rc
    OUTPUT_VARIABLE validate_out
    ERROR_VARIABLE validate_err
    TIMEOUT 15)
if(NOT validate_rc EQUAL 0 OR NOT validate_out MATCHES "settings_valid=1")
    message(FATAL_ERROR
        "configured output failed validate: rc=${validate_rc} "
        "out=${validate_out} err=${validate_err}")
endif()

message(STATUS "SearchEngineConfigBlock1Legacy: all checks passed")
