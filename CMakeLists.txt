cmake_minimum_required(VERSION 3.21)

# Make MSVC runtime/debug information target properties instead of editing
# CMAKE_*_FLAGS. This is important for Boost, GoogleTest and the application
# to use the same CRT in every configuration.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()

set(
        CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        CACHE STRING
        "MSVC CRT used by SearchEngine and all in-tree dependencies"
        FORCE
)
if(POLICY CMP0141)
    set(
            CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
            "$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>"
            CACHE STRING
            "MSVC debug information format"
            FORCE
    )
endif()

project(SearchEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(
            CACHE CMAKE_BUILD_TYPE
            PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel
    )
endif()

option(BUILD_SEARCHENGINE "Build the SearchEngine executable" ON)
option(BUILD_TESTS "Build legacy unit tests" OFF)
option(BUILD_ZAGEDITOR "Build the standalone ZagEditor tool" ON)
option(BUILD_BACKUP_SERVICE "Build the standalone BackupService tool" ON)
option(SEARCHENGINE_ENABLE_AVX2 "Require AVX2 instructions" OFF)
option(
        SEARCHENGINE_DISABLE_NINJA_MANIFEST
        "Disable the executable manifest for Ninja+MSVC (mt.exe workaround)"
        ON
)
set(
        SEARCHENGINE_WINDOWS_TARGET_VERSION
        "0x0A00"
        CACHE STRING
        "WINVER/_WIN32_WINNT value (0x0A00 is Windows 10)"
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(SEARCHENGINE_ARCH "x64")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(SEARCHENGINE_ARCH "x86")
else()
    message(FATAL_ERROR "Unsupported pointer size: ${CMAKE_SIZEOF_VOID_P}")
endif()

message(STATUS "SearchEngine target architecture: ${SEARCHENGINE_ARCH}")
message(STATUS "CMake generator: ${CMAKE_GENERATOR}")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
if(MSVC)
    message(STATUS "MSVC runtime: /MDd for Debug, /MD for other configurations")
    message(STATUS "MSVC iterator debug level: compiler default (2 for Debug, 0 otherwise)")
endif()

function(searchengine_configure_target target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_compile_definitions(${target_name} PRIVATE BOOST_ASIO_HAS_CO_AWAIT)

    if(WIN32)
        target_compile_definitions(
                ${target_name}
                PRIVATE
                "WINVER=${SEARCHENGINE_WINDOWS_TARGET_VERSION}"
                "_WIN32_WINNT=${SEARCHENGINE_WINDOWS_TARGET_VERSION}"
        )
    endif()

    if(MSVC)
        target_compile_options(
                ${target_name}
                PRIVATE
                /utf-8
                $<$<CONFIG:Debug>:/Od>
                $<$<CONFIG:Debug>:/Ob0>
                $<$<CONFIG:RelWithDebInfo>:/Zo>
        )

        if(SEARCHENGINE_ENABLE_AVX2)
            target_compile_options(${target_name} PRIVATE /arch:AVX2)
        endif()

        get_target_property(_target_type ${target_name} TYPE)
        if(_target_type STREQUAL "EXECUTABLE")
            target_link_options(
                    ${target_name}
                    PRIVATE
                    $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:/DEBUG:FULL>
            )
        endif()
    endif()

    set_target_properties(
            ${target_name}
            PROPERTIES
            COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols/$<CONFIG>"
            PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols/$<CONFIG>"
    )
endfunction()

function(searchengine_imported_location target_name configuration output_variable)
    get_target_property(
            _imported_location
            ${target_name}
            "IMPORTED_LOCATION_${configuration}"
    )
    if(NOT _imported_location OR _imported_location MATCHES "-NOTFOUND$")
        get_target_property(
                _imported_location
                ${target_name}
                "IMPORTED_IMPLIB_${configuration}"
        )
    endif()
    if(NOT _imported_location OR _imported_location MATCHES "-NOTFOUND$")
        get_target_property(_imported_location ${target_name} IMPORTED_LOCATION)
    endif()
    set(${output_variable} "${_imported_location}" PARENT_SCOPE)
endfunction()

function(searchengine_validate_boost_target target_name)
    if(NOT MSVC)
        return()
    endif()

    foreach(_configuration DEBUG RELEASE)
        searchengine_imported_location(
                ${target_name}
                ${_configuration}
                _boost_library
        )
        if(NOT _boost_library OR NOT EXISTS "${_boost_library}")
            message(
                    FATAL_ERROR
                    "${target_name} has no ${_configuration} library. "
                    "Boost must be built with variant=debug,release and "
                    "runtime-link=shared."
            )
        endif()

        if(SEARCHENGINE_ARCH STREQUAL "x64" AND _boost_library MATCHES "-x32-")
            message(FATAL_ERROR "${target_name} is x86, but the target is x64: ${_boost_library}")
        elseif(SEARCHENGINE_ARCH STREQUAL "x86" AND _boost_library MATCHES "-x64-")
            message(FATAL_ERROR "${target_name} is x64, but the target is x86: ${_boost_library}")
        endif()

        message(STATUS "${target_name} ${_configuration}: ${_boost_library}")
    endforeach()
endfunction()

set(_build_any_target OFF)
if(BUILD_SEARCHENGINE OR BUILD_TESTS OR BUILD_ZAGEDITOR OR BUILD_BACKUP_SERVICE)
    set(_build_any_target ON)
endif()

# ---------------------------------------------------------------------------
# Header-only dependencies
# ---------------------------------------------------------------------------
if(_build_any_target)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/lib/nlohmann_json/CMakeLists.txt")
        message(
                FATAL_ERROR
                "Vendored nlohmann_json is missing: "
                "${PROJECT_SOURCE_DIR}/lib/nlohmann_json"
        )
    endif()
    add_subdirectory(lib/nlohmann_json EXCLUDE_FROM_ALL)
endif()

# ---------------------------------------------------------------------------
# Boost
# ---------------------------------------------------------------------------
set(_boost_components headers)
if(BUILD_SEARCHENGINE OR BUILD_TESTS)
    list(APPEND _boost_components system filesystem serialization)
endif()
list(REMOVE_DUPLICATES _boost_components)

set(_default_boost_root "")
if(WIN32)
    if(SEARCHENGINE_ARCH STREQUAL "x64" AND EXISTS "C:/Boost/windows")
        set(_default_boost_root "C:/Boost/windows")
    elseif(SEARCHENGINE_ARCH STREQUAL "x86" AND EXISTS "C:/Boost/windows32")
        set(_default_boost_root "C:/Boost/windows32")
    endif()
endif()
set(
        SEARCHENGINE_BOOST_ROOT
        "${_default_boost_root}"
        CACHE PATH
        "Architecture-specific Boost install prefix"
)

# Discover the toolset tag from versioned MSVC library names. On the current
# workstation this selects vc143 for x64 and vc142 for the existing x86 build.
# Rebuilding x86 with cmake/BuildBoost.ps1 changes this automatically to vc143.
set(_default_boost_compiler "")
if(MSVC AND SEARCHENGINE_BOOST_ROOT AND (BUILD_SEARCHENGINE OR BUILD_TESTS))
    file(
            GLOB
            _boost_serialization_variants
            "${SEARCHENGINE_BOOST_ROOT}/lib/libboost_serialization-vc*-mt-*.lib"
    )
    set(_boost_compiler_tags "")
    foreach(_boost_variant IN LISTS _boost_serialization_variants)
        get_filename_component(_boost_variant_name "${_boost_variant}" NAME)
        if(_boost_variant_name MATCHES "-(vc[0-9]+)-")
            list(APPEND _boost_compiler_tags "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _boost_compiler_tags)
    list(LENGTH _boost_compiler_tags _boost_compiler_tag_count)
    if(_boost_compiler_tag_count EQUAL 1)
        list(GET _boost_compiler_tags 0 _default_boost_compiler)
    endif()
endif()
set(
        SEARCHENGINE_BOOST_COMPILER
        "${_default_boost_compiler}"
        CACHE STRING
        "Boost.Build toolset tag, for example vc143 or vc142"
)

if(_build_any_target)
    set(Boost_USE_STATIC_LIBS ON)
    set(Boost_USE_STATIC_RUNTIME OFF)
    set(Boost_USE_MULTITHREADED ON)

    if(SEARCHENGINE_BOOST_COMPILER)
        set(Boost_COMPILER "${SEARCHENGINE_BOOST_COMPILER}")
    endif()

    if(SEARCHENGINE_BOOST_ROOT)
        find_package(
                Boost 1.85 CONFIG REQUIRED
                COMPONENTS ${_boost_components}
                PATHS "${SEARCHENGINE_BOOST_ROOT}"
                NO_DEFAULT_PATH
        )
    else()
        find_package(
                Boost 1.85 CONFIG REQUIRED
                COMPONENTS ${_boost_components}
        )
    endif()

    message(STATUS "Boost version: ${Boost_VERSION}")
    message(STATUS "Boost root: ${SEARCHENGINE_BOOST_ROOT}")
    message(STATUS "Boost compiler tag: ${SEARCHENGINE_BOOST_COMPILER}")

    if(BUILD_SEARCHENGINE OR BUILD_TESTS)
        searchengine_validate_boost_target(Boost::system)
        searchengine_validate_boost_target(Boost::filesystem)
        searchengine_validate_boost_target(Boost::serialization)
    endif()
endif()

# ---------------------------------------------------------------------------
# Architecture-neutral utf8proc build
# ---------------------------------------------------------------------------
set(_default_utf8proc_source "")
foreach(
        _utf8proc_candidate
        "${PROJECT_SOURCE_DIR}/utf8proc-2.10.0/utf8proc-2.10.0"
        "${PROJECT_SOURCE_DIR}/utf8proc"
)
    if(EXISTS "${_utf8proc_candidate}/utf8proc.c")
        set(_default_utf8proc_source "${_utf8proc_candidate}")
        break()
    endif()
endforeach()
set(
        SEARCHENGINE_UTF8PROC_SOURCE_DIR
        "${_default_utf8proc_source}"
        CACHE PATH
        "Directory containing utf8proc.c and utf8proc.h"
)
set(
        SEARCHENGINE_UTF8PROC_LIBRARY
        ""
        CACHE FILEPATH
        "Prebuilt utf8proc library (used only when source is unavailable)"
)
set(
        SEARCHENGINE_UTF8PROC_INCLUDE_DIR
        ""
        CACHE PATH
        "Include directory for a prebuilt utf8proc library"
)

if(BUILD_SEARCHENGINE)
    if(
            SEARCHENGINE_UTF8PROC_SOURCE_DIR
            AND EXISTS "${SEARCHENGINE_UTF8PROC_SOURCE_DIR}/utf8proc.c"
            AND EXISTS "${SEARCHENGINE_UTF8PROC_SOURCE_DIR}/utf8proc.h"
    )
        add_library(
                searchengine_utf8proc
                STATIC
                "${SEARCHENGINE_UTF8PROC_SOURCE_DIR}/utf8proc.c"
                "${SEARCHENGINE_UTF8PROC_SOURCE_DIR}/utf8proc.h"
        )
        add_library(SearchEngine::utf8proc ALIAS searchengine_utf8proc)
        target_include_directories(
                searchengine_utf8proc
                PUBLIC
                "${SEARCHENGINE_UTF8PROC_SOURCE_DIR}"
        )
        target_compile_definitions(
                searchengine_utf8proc
                PUBLIC UTF8PROC_STATIC
                PRIVATE UTF8PROC_EXPORTS
        )
        set_target_properties(
                searchengine_utf8proc
                PROPERTIES POSITION_INDEPENDENT_CODE ON
        )
        message(STATUS "utf8proc: building from ${SEARCHENGINE_UTF8PROC_SOURCE_DIR}")
    elseif(
            SEARCHENGINE_UTF8PROC_LIBRARY
            AND EXISTS "${SEARCHENGINE_UTF8PROC_LIBRARY}"
            AND SEARCHENGINE_UTF8PROC_INCLUDE_DIR
            AND EXISTS "${SEARCHENGINE_UTF8PROC_INCLUDE_DIR}/utf8proc.h"
    )
        add_library(SearchEngine_utf8proc STATIC IMPORTED GLOBAL)
        add_library(SearchEngine::utf8proc ALIAS SearchEngine_utf8proc)
        set_target_properties(
                SearchEngine_utf8proc
                PROPERTIES
                IMPORTED_LOCATION "${SEARCHENGINE_UTF8PROC_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${SEARCHENGINE_UTF8PROC_INCLUDE_DIR}"
                INTERFACE_COMPILE_DEFINITIONS UTF8PROC_STATIC
        )
        message(STATUS "utf8proc: using prebuilt ${SEARCHENGINE_UTF8PROC_LIBRARY}")
    else()
        message(
                FATAL_ERROR
                "utf8proc source was not found. Set SEARCHENGINE_UTF8PROC_SOURCE_DIR "
                "to a utf8proc source tree. A single prebuilt x86 .lib cannot be "
                "used for both Win32 and x64."
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# OpenSSL (FindOpenSSL chooses MDd/MD and x86/x64 paths per configuration)
# ---------------------------------------------------------------------------
if(BUILD_SEARCHENGINE)
    set(_default_openssl_root "")
    if(WIN32)
        if(
                SEARCHENGINE_ARCH STREQUAL "x64"
                AND EXISTS "C:/Program Files/OpenSSL-Win64"
        )
            set(_default_openssl_root "C:/Program Files/OpenSSL-Win64")
        elseif(
                SEARCHENGINE_ARCH STREQUAL "x86"
                AND EXISTS "C:/Program Files (x86)/OpenSSL-Win32"
        )
            set(_default_openssl_root "C:/Program Files (x86)/OpenSSL-Win32")
        endif()
    endif()
    set(
            SEARCHENGINE_OPENSSL_ROOT
            "${_default_openssl_root}"
            CACHE PATH
            "Architecture-specific OpenSSL install prefix"
    )

    if(SEARCHENGINE_OPENSSL_ROOT)
        set(OPENSSL_ROOT_DIR "${SEARCHENGINE_OPENSSL_ROOT}")
    endif()
    set(OPENSSL_USE_STATIC_LIBS OFF)
    set(OPENSSL_MSVC_STATIC_RT OFF)
    find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)

    searchengine_imported_location(OpenSSL::SSL DEBUG _openssl_ssl_debug)
    searchengine_imported_location(OpenSSL::SSL RELEASE _openssl_ssl_release)
    searchengine_imported_location(OpenSSL::Crypto DEBUG _openssl_crypto_debug)
    searchengine_imported_location(OpenSSL::Crypto RELEASE _openssl_crypto_release)
    message(STATUS "OpenSSL root: ${SEARCHENGINE_OPENSSL_ROOT}")
    message(STATUS "OpenSSL SSL Debug: ${_openssl_ssl_debug}")
    message(STATUS "OpenSSL SSL Release: ${_openssl_ssl_release}")
    message(STATUS "OpenSSL Crypto Debug: ${_openssl_crypto_debug}")
    message(STATUS "OpenSSL Crypto Release: ${_openssl_crypto_release}")
endif()

# ---------------------------------------------------------------------------
# SearchEngine
# ---------------------------------------------------------------------------
if(BUILD_SEARCHENGINE)
    file(
            GLOB_RECURSE
            searchengine_sources
            CONFIGURE_DEPENDS
            "${PROJECT_SOURCE_DIR}/src/*.cpp"
            "${PROJECT_SOURCE_DIR}/src/*.c"
    )
    list(
            FILTER searchengine_sources
            EXCLUDE REGEX
            "/src/Backup/|/src/scheduler/BackupTask\\.cpp$"
    )

    add_executable(SearchEngine ${searchengine_sources} main.cpp)
    target_include_directories(
            SearchEngine
            PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/lib"
            "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
            SearchEngine
            PRIVATE
            nlohmann_json::nlohmann_json
            Boost::headers
            Boost::system
            Boost::filesystem
            Boost::serialization
            OpenSSL::Crypto
            OpenSSL::SSL
            SearchEngine::utf8proc
    )
    if(WIN32)
        target_link_libraries(SearchEngine PRIVATE ws2_32)
    endif()
    searchengine_configure_target(SearchEngine)

    if(
            MSVC
            AND CMAKE_GENERATOR MATCHES "Ninja"
            AND SEARCHENGINE_DISABLE_NINJA_MANIFEST
    )
        target_link_options(SearchEngine PRIVATE /MANIFEST:NO)
    endif()

    find_program(_powershell_executable NAMES pwsh powershell)
    if(_powershell_executable)
        add_custom_command(
                TARGET SearchEngine
                POST_BUILD
                COMMAND
                "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:SearchEngine>/out"
                COMMAND
                "${_powershell_executable}"
                -NoProfile
                -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/copy_with_version.ps1"
                "$<TARGET_FILE:SearchEngine>"
                "$<TARGET_FILE_DIR:SearchEngine>/out"
                SearchEngine
                VERBATIM
        )
    else()
        message(WARNING "PowerShell was not found; versioned post-build copy is disabled")
    endif()
endif()

# ---------------------------------------------------------------------------
# BackupService
# ---------------------------------------------------------------------------
if(BUILD_BACKUP_SERVICE)
    add_executable(
            BackupService
            tools/backup/main.cpp
            tools/backup/BackupServiceApplication.cpp
            tools/backup/BackupServiceOptions.cpp
            tools/backup/BackupWindowsService.cpp
            src/Backup/BackupCache.cpp
            src/Backup/BackupConfig.cpp
            src/Backup/BackupEngine.cpp
            src/Backup/FileHash.cpp
            src/Backup/MirrorHistory.cpp
            src/Backup/SQLiteBackup.cpp
            lib/backup_sqlite/sqlite3.c
            src/scheduler/AbstractScheduledTask.cpp
            src/scheduler/BackupTask.cpp
            src/MyUtils/LogFile.cpp
            src/MyUtils/Encoding.cpp
    )
    target_include_directories(
            BackupService
            PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/lib"
            "${PROJECT_SOURCE_DIR}/lib/backup_sqlite"
            "${PROJECT_SOURCE_DIR}/src"
            "${PROJECT_SOURCE_DIR}/tools/backup"
    )
    target_link_libraries(
            BackupService
            PRIVATE
            nlohmann_json::nlohmann_json
            Boost::headers
    )
    target_compile_definitions(
            BackupService
            PRIVATE
            BOOST_ALL_NO_LIB
            BOOST_ERROR_CODE_HEADER_ONLY
            BOOST_SYSTEM_NO_LIB
    )
    if(WIN32)
        target_link_libraries(BackupService PRIVATE ws2_32 advapi32)
    endif()
    searchengine_configure_target(BackupService)
endif()

# ---------------------------------------------------------------------------
# ZagEditor
# ---------------------------------------------------------------------------
if(BUILD_ZAGEDITOR)
    add_executable(
            ZagEditor
            tools/zag_editor/main.cpp
            tools/zag_editor/ZagEditorSettings.cpp
            tools/zag_editor/ZagKpodiChanger.cpp
            tools/zag_editor/ZagProcessCommand.cpp
            tools/zag_editor/ZagEditorLog.cpp
            src/FileWatcher/FileWatcher.cpp
            src/FileWatcher/MultiWatcher.cpp
            src/FileWatcher/FileEventDispatcher.cpp
            src/scheduler/AbstractScheduledTask.cpp
            src/ContextRuntime/ContextRuntime.cpp
            src/MyUtils/LogFile.cpp
            src/MyUtils/Encoding.cpp
    )
    target_include_directories(
            ZagEditor
            PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/lib"
            "${PROJECT_SOURCE_DIR}/src"
            "${PROJECT_SOURCE_DIR}/tools/zag_editor"
    )
    target_link_libraries(
            ZagEditor
            PRIVATE
            nlohmann_json::nlohmann_json
            Boost::headers
    )
    target_compile_definitions(
            ZagEditor
            PRIVATE
            BOOST_ALL_NO_LIB
            BOOST_ERROR_CODE_HEADER_ONLY
            BOOST_SYSTEM_NO_LIB
    )
    if(WIN32)
        target_link_libraries(ZagEditor PRIVATE ws2_32)
    endif()
    searchengine_configure_target(ZagEditor)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(BUILD_TESTS)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/lib/googletest/CMakeLists.txt")
        message(
                FATAL_ERROR
                "Vendored GoogleTest is missing: ${PROJECT_SOURCE_DIR}/lib/googletest"
        )
    endif()

    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(lib/googletest EXCLUDE_FROM_ALL)

    # This repository carries an old GoogleTest snapshot. MSVC 2022 diagnoses
    # several of its legacy printf checks as C4477, while that snapshot adds
    # /WX to its own targets. Keep application diagnostics unchanged and
    # relax warnings-as-errors only for the third-party library.
    if(MSVC)
        foreach(_gtest_target gtest gtest_main)
            if(TARGET ${_gtest_target})
                target_compile_options(${_gtest_target} PRIVATE /WX- /wd4477)
            endif()
        endforeach()
    endif()

    enable_testing()

    file(
            GLOB_RECURSE
            test_sources
            CONFIGURE_DEPENDS
            "${PROJECT_SOURCE_DIR}/test/*.cpp"
            "${PROJECT_SOURCE_DIR}/tests/*.cpp"
    )
    if(NOT test_sources)
        message(
                WARNING
                "BUILD_TESTS is ON, but no *.cpp files were found under test/ "
                "or tests/. SearchEngine_test will contain "
                "only GoogleTest's main() and CTest will report no tests."
        )
    endif()
    set(
            shared_test_sources
              "${PROJECT_SOURCE_DIR}/src/Backup/BackupConfig.cpp"
              "${PROJECT_SOURCE_DIR}/src/Backup/BackupCache.cpp"
              "${PROJECT_SOURCE_DIR}/src/Backup/BackupEngine.cpp"
            "${PROJECT_SOURCE_DIR}/src/Backup/FileHash.cpp"
            "${PROJECT_SOURCE_DIR}/src/Backup/MirrorHistory.cpp"
            "${PROJECT_SOURCE_DIR}/src/Backup/SQLiteBackup.cpp"
            "${PROJECT_SOURCE_DIR}/lib/backup_sqlite/sqlite3.c"
            "${PROJECT_SOURCE_DIR}/src/MyUtils/OEMCase.cpp"
            "${PROJECT_SOURCE_DIR}/src/MyUtils/Encoding.cpp"
            "${PROJECT_SOURCE_DIR}/src/MyUtils/LogFile.cpp"
            "${PROJECT_SOURCE_DIR}/src/scheduler/AbstractScheduledTask.cpp"
            "${PROJECT_SOURCE_DIR}/src/scheduler/BackupTask.cpp"
            "${PROJECT_SOURCE_DIR}/tools/backup/BackupServiceApplication.cpp"
            "${PROJECT_SOURCE_DIR}/tools/backup/BackupServiceOptions.cpp"
    )

    add_executable(SearchEngine_test ${test_sources} ${shared_test_sources})
    target_include_directories(
            SearchEngine_test
            PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/lib"
            "${PROJECT_SOURCE_DIR}/lib/backup_sqlite"
            "${PROJECT_SOURCE_DIR}/src"
            "${PROJECT_SOURCE_DIR}/tools/backup"
    )
    target_link_libraries(
            SearchEngine_test
            PRIVATE
            GTest::gtest
            GTest::gtest_main
            nlohmann_json::nlohmann_json
            Boost::headers
            Boost::system
            Boost::filesystem
            Boost::serialization
    )
    if(WIN32)
        target_link_libraries(SearchEngine_test PRIVATE ws2_32)
    endif()
    searchengine_configure_target(SearchEngine_test)
    add_test(NAME SearchEngine_test COMMAND SearchEngine_test)
endif()
