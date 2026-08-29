if(NOT DEFINED CONFIG_EXE OR NOT EXISTS "${CONFIG_EXE}")
    message(FATAL_ERROR "CONFIG_EXE is missing: ${CONFIG_EXE}")
endif()

execute_process(
    COMMAND "${CONFIG_EXE}" validate-script-messages
    RESULT_VARIABLE catalog_result
    OUTPUT_VARIABLE catalog_output
    ERROR_VARIABLE catalog_error
)
if(NOT catalog_result EQUAL 0)
    message(FATAL_ERROR "Script message catalog validation failed: ${catalog_error}")
endif()
string(FIND "${catalog_output}" "script_messages_valid=1" catalog_position)
if(catalog_position LESS 0)
    message(FATAL_ERROR "Script message catalog validation marker is missing")
endif()

execute_process(
    COMMAND "${CONFIG_EXE}" script-message --language ru --id common.select
    ENCODING UTF-8
    RESULT_VARIABLE ru_result
    OUTPUT_VARIABLE ru_output
    ERROR_VARIABLE ru_error
)
if(NOT ru_result EQUAL 0)
    message(FATAL_ERROR "Russian script message failed: ${ru_error}")
endif()
string(FIND "${ru_output}" "Ваш выбор" ru_position)
if(ru_position LESS 0)
    message(FATAL_ERROR "Russian script message is missing Russian text: ${ru_output}")
endif()
string(FIND "${ru_output}" "Select" ru_english_position)
if(NOT ru_english_position LESS 0)
    message(FATAL_ERROR "Russian script message contains English prompt: ${ru_output}")
endif()

execute_process(
    COMMAND "${CONFIG_EXE}" script-message --language en --id common.select
    ENCODING UTF-8
    RESULT_VARIABLE en_result
    OUTPUT_VARIABLE en_output
    ERROR_VARIABLE en_error
)
if(NOT en_result EQUAL 0)
    message(FATAL_ERROR "English script message failed: ${en_error}")
endif()
string(FIND "${en_output}" "Select" en_position)
if(en_position LESS 0)
    message(FATAL_ERROR "English script message is missing English text: ${en_output}")
endif()
string(FIND "${en_output}" "Ваш выбор" en_russian_position)
if(NOT en_russian_position LESS 0)
    message(FATAL_ERROR "English script message contains Russian prompt: ${en_output}")
endif()

execute_process(
    COMMAND "${CONFIG_EXE}" script-message --language ru
            --id uninstall.success --arg1 SearchEngineService-test
    ENCODING UTF-8
    RESULT_VARIABLE placeholder_result
    OUTPUT_VARIABLE placeholder_output
    ERROR_VARIABLE placeholder_error
)
if(NOT placeholder_result EQUAL 0)
    message(FATAL_ERROR "Placeholder script message failed: ${placeholder_error}")
endif()
string(FIND "${placeholder_output}" "SearchEngineService-test" placeholder_position)
if(placeholder_position LESS 0)
    message(FATAL_ERROR "Script message placeholder was not substituted: ${placeholder_output}")
endif()
string(FIND "${placeholder_output}" "{1}" unresolved_position)
if(NOT unresolved_position LESS 0)
    message(FATAL_ERROR "Script message contains unresolved placeholder: ${placeholder_output}")
endif()

execute_process(
    COMMAND "${CONFIG_EXE}" script-message --language ru --id missing.message
    ENCODING UTF-8
    RESULT_VARIABLE missing_result
    OUTPUT_VARIABLE missing_output
    ERROR_VARIABLE missing_error
)
if(missing_result EQUAL 0)
    message(FATAL_ERROR "Unknown script message id was accepted")
endif()

message(STATUS "SearchEngineConfigScriptMessages: all checks passed")
