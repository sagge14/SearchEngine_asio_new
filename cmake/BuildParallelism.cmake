include(ProcessorCount)
ProcessorCount(_searchengine_processor_count)
if(_searchengine_processor_count LESS 1)
    set(_searchengine_processor_count 1)
endif()

set(SEARCHENGINE_BUILD_JOBS 0 CACHE STRING
    "Maximum compiler processes; 0 uses floor(90% of available processors)")
if(NOT SEARCHENGINE_BUILD_JOBS MATCHES "^[0-9]+$")
    message(FATAL_ERROR "SEARCHENGINE_BUILD_JOBS must be a non-negative integer")
endif()

if(SEARCHENGINE_BUILD_JOBS EQUAL 0)
    math(EXPR SEARCHENGINE_COMPILE_JOBS "${_searchengine_processor_count} * 9 / 10")
    if(SEARCHENGINE_COMPILE_JOBS LESS 1)
        set(SEARCHENGINE_COMPILE_JOBS 1)
    endif()
else()
    set(SEARCHENGINE_COMPILE_JOBS "${SEARCHENGINE_BUILD_JOBS}")
endif()

if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
    # Share one compiler process budget across all projects. /m and /MP alone
    # would multiply their independent limits and oversubscribe the machine.
    list(FILTER CMAKE_VS_GLOBALS EXCLUDE REGEX
        "^(UseMultiToolTask|EnforceProcessCountAcrossBuilds|CL_MPCount|MultiProcMaxCount)=")
    list(APPEND CMAKE_VS_GLOBALS
        "UseMultiToolTask=true"
        "EnforceProcessCountAcrossBuilds=true"
        "CL_MPCount=${SEARCHENGINE_COMPILE_JOBS}"
        "MultiProcMaxCount=${SEARCHENGINE_COMPILE_JOBS}")
    message(STATUS
        "SearchEngine parallel compilation: ${SEARCHENGINE_COMPILE_JOBS} processes (${_searchengine_processor_count} processors)")
endif()
