if(NOT DEFINED CONFIG_EXE OR NOT DEFINED TEMPLATE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "CONFIG_EXE, TEMPLATE and TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")

# ---------------------------------------------------------------
# Setup: build a fake data-dir with managed and sentinel files
# ---------------------------------------------------------------
set(data_dir "${TEST_ROOT}/data-dir")
file(MAKE_DIRECTORY "${data_dir}")
file(MAKE_DIRECTORY "${data_dir}/logs")
file(MAKE_DIRECTORY "${data_dir}/messages")

# Settings.json - original content
set(old_settings_content "{ \"old\": true }")
file(WRITE "${data_dir}/Settings.json" "${old_settings_content}")

# client-endpoint.txt - original endpoint
set(old_endpoint_content "port=15001\ngod=2024\nhost=localhost\n")
file(WRITE "${data_dir}/client-endpoint.txt" "${old_endpoint_content}")

# Sentinel files - must remain unchanged after apply/rollback
file(WRITE "${data_dir}/logs/sentinel"          "LOGS_SENTINEL_BYTES")
file(WRITE "${data_dir}/messages/sentinel"      "MESSAGES_SENTINEL_BYTES")
file(WRITE "${data_dir}/auth_clients.sqlite"    "SQLITE_DUMMY")
file(WRITE "${data_dir}/inverted_index.sqlite"  "INDEX_DUMMY")
file(WRITE "${data_dir}/prefix_map.json"        "{\"sentinel\":1}")
file(WRITE "${data_dir}/unknown-user-file.dat"  "USER_DATA")

# New Settings.json (replacement)
set(new_settings_content "{ \"new\": true }")
file(WRITE "${TEST_ROOT}/new-settings.json" "${new_settings_content}")

# New endpoint (replacement)
set(new_endpoint_content "port=15002\ngod=2025\nhost=localhost\n")
file(WRITE "${TEST_ROOT}/new-endpoint.txt" "${new_endpoint_content}")

# Rollback directory (must not yet exist)
set(rollback_dir "${TEST_ROOT}/rollback")

# ---------------------------------------------------------------
# Helper: read file content
# ---------------------------------------------------------------
macro(read_file_content path outvar)
    file(READ "${path}" ${outvar})
endmacro()

# ---------------------------------------------------------------
# Step 1: Apply transaction (Settings + endpoint)
# ---------------------------------------------------------------
execute_process(
    COMMAND "${CONFIG_EXE}" settings-transaction-apply
        --data-dir "${data_dir}"
        --settings-temp "${TEST_ROOT}/new-settings.json"
        --rollback-dir "${rollback_dir}"
        --endpoint-temp "${TEST_ROOT}/new-endpoint.txt"
    RESULT_VARIABLE apply_rc
    OUTPUT_VARIABLE apply_out
    ERROR_VARIABLE  apply_err
    TIMEOUT 30)

if(NOT apply_rc EQUAL 0)
    message(FATAL_ERROR
        "settings-transaction-apply failed (rc=${apply_rc})\n"
        "stdout: ${apply_out}\nstderr: ${apply_err}")
endif()

# Verify Settings.json was replaced
read_file_content("${data_dir}/Settings.json" actual_settings)
if(NOT actual_settings STREQUAL "${new_settings_content}")
    message(FATAL_ERROR
        "Settings.json was not replaced correctly after apply.\n"
        "Expected: '${new_settings_content}'\nActual: '${actual_settings}'")
endif()

# Verify client-endpoint.txt was replaced
read_file_content("${data_dir}/client-endpoint.txt" actual_endpoint)
if(NOT actual_endpoint STREQUAL "${new_endpoint_content}")
    message(FATAL_ERROR
        "client-endpoint.txt was not replaced correctly after apply.\n"
        "Expected: '${new_endpoint_content}'\nActual: '${actual_endpoint}'")
endif()

# Verify rollback dir was created
if(NOT EXISTS "${rollback_dir}")
    message(FATAL_ERROR "rollback-dir was not created by apply")
endif()

# Verify snapshot files exist
if(NOT EXISTS "${rollback_dir}/Settings.json.snapshot")
    message(FATAL_ERROR "Settings.json.snapshot is missing from rollback-dir")
endif()
if(NOT EXISTS "${rollback_dir}/client-endpoint.txt.snapshot")
    message(FATAL_ERROR "client-endpoint.txt.snapshot is missing from rollback-dir")
endif()

# Verify sentinel files were NOT touched
read_file_content("${data_dir}/logs/sentinel" sentinel_logs)
if(NOT sentinel_logs STREQUAL "LOGS_SENTINEL_BYTES")
    message(FATAL_ERROR "logs/sentinel was modified by apply")
endif()

read_file_content("${data_dir}/messages/sentinel" sentinel_messages)
if(NOT sentinel_messages STREQUAL "MESSAGES_SENTINEL_BYTES")
    message(FATAL_ERROR "messages/sentinel was modified by apply")
endif()

read_file_content("${data_dir}/auth_clients.sqlite" sentinel_auth)
if(NOT sentinel_auth STREQUAL "SQLITE_DUMMY")
    message(FATAL_ERROR "auth_clients.sqlite was modified by apply")
endif()

read_file_content("${data_dir}/inverted_index.sqlite" sentinel_index)
if(NOT sentinel_index STREQUAL "INDEX_DUMMY")
    message(FATAL_ERROR "inverted_index.sqlite was modified by apply")
endif()

read_file_content("${data_dir}/prefix_map.json" sentinel_prefix)
if(NOT sentinel_prefix STREQUAL "{\"sentinel\":1}")
    message(FATAL_ERROR "prefix_map.json was modified by apply")
endif()

read_file_content("${data_dir}/unknown-user-file.dat" sentinel_user)
if(NOT sentinel_user STREQUAL "USER_DATA")
    message(FATAL_ERROR "unknown-user-file.dat was modified by apply")
endif()

message(STATUS "Apply step: OK")

# ---------------------------------------------------------------
# Step 2: Rollback
# ---------------------------------------------------------------
execute_process(
    COMMAND "${CONFIG_EXE}" settings-transaction-rollback
        --data-dir "${data_dir}"
        --rollback-dir "${rollback_dir}"
    RESULT_VARIABLE rollback_rc
    OUTPUT_VARIABLE rollback_out
    ERROR_VARIABLE  rollback_err
    TIMEOUT 30)

if(NOT rollback_rc EQUAL 0)
    message(FATAL_ERROR
        "settings-transaction-rollback failed (rc=${rollback_rc})\n"
        "stdout: ${rollback_out}\nstderr: ${rollback_err}")
endif()

# Verify Settings.json was restored byte-for-byte
read_file_content("${data_dir}/Settings.json" restored_settings)
if(NOT restored_settings STREQUAL "${old_settings_content}")
    message(FATAL_ERROR
        "Settings.json was not restored correctly after rollback.\n"
        "Expected: '${old_settings_content}'\nActual: '${restored_settings}'")
endif()

# Verify client-endpoint.txt was restored byte-for-byte
read_file_content("${data_dir}/client-endpoint.txt" restored_endpoint)
if(NOT restored_endpoint STREQUAL "${old_endpoint_content}")
    message(FATAL_ERROR
        "client-endpoint.txt was not restored correctly after rollback.\n"
        "Expected: '${old_endpoint_content}'\nActual: '${restored_endpoint}'")
endif()

# Verify sentinel files still unchanged after rollback
read_file_content("${data_dir}/logs/sentinel" sentinel_logs2)
if(NOT sentinel_logs2 STREQUAL "LOGS_SENTINEL_BYTES")
    message(FATAL_ERROR "logs/sentinel was modified by rollback")
endif()

read_file_content("${data_dir}/auth_clients.sqlite" sentinel_auth2)
if(NOT sentinel_auth2 STREQUAL "SQLITE_DUMMY")
    message(FATAL_ERROR "auth_clients.sqlite was modified by rollback")
endif()

message(STATUS "Rollback step: OK")

# ---------------------------------------------------------------
# Step 3: Commit (cleanup rollback dir)
# ---------------------------------------------------------------
execute_process(
    COMMAND "${CONFIG_EXE}" settings-transaction-commit
        --data-dir "${data_dir}"
        --rollback-dir "${rollback_dir}"
    RESULT_VARIABLE commit_rc
    OUTPUT_VARIABLE commit_out
    ERROR_VARIABLE  commit_err
    TIMEOUT 30)

if(NOT commit_rc EQUAL 0)
    message(FATAL_ERROR
        "settings-transaction-commit failed (rc=${commit_rc})\n"
        "stdout: ${commit_out}\nstderr: ${commit_err}")
endif()

# Verify rollback dir was removed
if(EXISTS "${rollback_dir}")
    message(FATAL_ERROR "rollback-dir still exists after commit")
endif()

message(STATUS "Commit step: OK")

# ---------------------------------------------------------------
# Step 4: Apply without endpoint (Settings only)
# ---------------------------------------------------------------
set(rollback_dir2 "${TEST_ROOT}/rollback2")
file(WRITE "${data_dir}/Settings.json" "${old_settings_content}")

execute_process(
    COMMAND "${CONFIG_EXE}" settings-transaction-apply
        --data-dir "${data_dir}"
        --settings-temp "${TEST_ROOT}/new-settings.json"
        --rollback-dir "${rollback_dir2}"
    RESULT_VARIABLE apply2_rc
    OUTPUT_VARIABLE apply2_out
    ERROR_VARIABLE  apply2_err
    TIMEOUT 30)

if(NOT apply2_rc EQUAL 0)
    message(FATAL_ERROR
        "settings-only apply failed (rc=${apply2_rc})\n"
        "stdout: ${apply2_out}\nstderr: ${apply2_err}")
endif()

# endpoint should be unchanged
read_file_content("${data_dir}/client-endpoint.txt" ep2)
if(NOT ep2 STREQUAL "${old_endpoint_content}")
    message(FATAL_ERROR
        "client-endpoint.txt was unexpectedly modified in settings-only apply")
endif()

# No endpoint snapshot should exist
if(EXISTS "${rollback_dir2}/client-endpoint.txt.snapshot")
    message(FATAL_ERROR
        "endpoint snapshot present for settings-only apply")
endif()

# Commit cleanup
execute_process(
    COMMAND "${CONFIG_EXE}" settings-transaction-commit
        --data-dir "${data_dir}"
        --rollback-dir "${rollback_dir2}"
    RESULT_VARIABLE commit2_rc TIMEOUT 15)
if(NOT commit2_rc EQUAL 0)
    message(FATAL_ERROR "settings-only commit failed (rc=${commit2_rc})")
endif()

message(STATUS "Settings-only apply/commit: OK")

# ---------------------------------------------------------------
# Step 5: Negative scenario — rollback with corrupt/missing snapshot
# Apply succeeds; then delete the Settings snapshot to simulate
# a partially-corrupt rollback-dir; rollback must fail (non-zero).
# Rollback-dir must NOT be cleaned up by the helper on failure.
# ---------------------------------------------------------------
set(rollback_dir3 "${TEST_ROOT}/rollback3")
file(WRITE "${data_dir}/Settings.json" "${old_settings_content}")

execute_process(
    COMMAND "${CONFIG_EXE}" settings-transaction-apply
        --data-dir "${data_dir}"
        --settings-temp "${TEST_ROOT}/new-settings.json"
        --rollback-dir "${rollback_dir3}"
    RESULT_VARIABLE apply3_rc TIMEOUT 30)
if(NOT apply3_rc EQUAL 0)
    message(FATAL_ERROR "Negative-scenario apply failed unexpectedly (rc=${apply3_rc})")
endif()

# Corrupt the rollback-dir by removing the snapshot
file(REMOVE "${rollback_dir3}/Settings.json.snapshot")

execute_process(
    COMMAND "${CONFIG_EXE}" settings-transaction-rollback
        --data-dir "${data_dir}"
        --rollback-dir "${rollback_dir3}"
    RESULT_VARIABLE rollback3_rc
    OUTPUT_VARIABLE rollback3_out
    ERROR_VARIABLE  rollback3_err
    TIMEOUT 30)

if(rollback3_rc EQUAL 0)
    message(FATAL_ERROR
        "Negative rollback should have FAILED but returned 0\n"
        "stdout: ${rollback3_out}\nstderr: ${rollback3_err}")
endif()

# Rollback-dir must still exist (snapshots preserved for manual recovery)
if(NOT EXISTS "${rollback_dir3}")
    message(FATAL_ERROR "rollback-dir was removed after failed rollback; snapshots must be preserved")
endif()

message(STATUS "Negative rollback scenario: OK (rollback-dir preserved on failure)")

# Cleanup the corrupt rollback-dir for test isolation
file(REMOVE_RECURSE "${rollback_dir3}")

message(STATUS "SearchEngineConfigSettingsTransaction: all checks passed")
