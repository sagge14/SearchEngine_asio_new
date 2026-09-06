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

include("${CMAKE_CURRENT_LIST_DIR}/cmake/Win32VersionInfo.cmake")

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
option(BUILD_CONFIG_TOOL "Build the portable SearchEngineConfig helper" ${WIN32})
option(BUILD_ARCHIVE_TOOL "Build the SearchEngineArchive migration helper" ${WIN32})
option(
        BUILD_ARCHIVE_E2E_STAND_TOOL
        "Build the disposable SearchEngine archive E2E stand generator"
        OFF
)
option(BUILD_TESTS "Build legacy unit tests" OFF)
option(BUILD_ZAGEDITOR "Build the standalone ZagEditor tool" ON)
option(BUILD_BACKUP_SERVICE "Build the standalone BackupService tool" ON)
option(BUILD_BACKUP_RESTORE "Build the BackupRestore CLI and core library" ON)
option(BUILD_AUTH_DB_TOOL "Build the AuthDbTool SQLite client admin CLI" ON)
option(BUILD_TOKEN_ISSUER "Build the SearchClientTokenIssuer USB/computer token CLI" ${WIN32})
option(
        SEARCHENGINE_PACKAGE_ON_RELEASE_BUILD
        "After Release build of SearchEngine/BackupService/ZagEditor/BackupRestore in packagable presets (windows-x64, windows-x86, windows7-x86), run New-*Package and cloud-publish when configured"
        ON
)
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
if(BUILD_SEARCHENGINE OR BUILD_CONFIG_TOOL OR BUILD_ARCHIVE_TOOL OR BUILD_ARCHIVE_E2E_STAND_TOOL OR BUILD_TESTS OR BUILD_ZAGEDITOR OR BUILD_BACKUP_SERVICE OR BUILD_BACKUP_RESTORE OR BUILD_AUTH_DB_TOOL)
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
# Portable configuration and installation helper
# ---------------------------------------------------------------------------
if(BUILD_CONFIG_TOOL)
    if(NOT WIN32)
        message(FATAL_ERROR "SearchEngineConfig currently supports Windows only")
    endif()

    add_executable(
            SearchEngineConfig
            tools/config/main.cpp
            tools/config/ScriptMessages.cpp
            tools/config/RuntimeDataTransaction.cpp
            src/MyUtils/FileExtensionContract.cpp
            src/SearchServer/QueryWordMatch.cpp
    )
    target_include_directories(
            SearchEngineConfig
            PRIVATE
            "${PROJECT_SOURCE_DIR}/lib"
            "${PROJECT_SOURCE_DIR}/src"
            "${PROJECT_SOURCE_DIR}/tools/config"
    )
    target_link_libraries(
            SearchEngineConfig
            PRIVATE
            nlohmann_json::nlohmann_json
            ws2_32
            advapi32
    )
    target_compile_definitions(
            SearchEngineConfig
            PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )
    searchengine_configure_target(SearchEngineConfig)
    searchengine_target_win32_versioninfo(
            SearchEngineConfig
            PRODUCT_NAME "SearchEngineService"
            ORIGINAL_FILENAME "SearchEngineConfig.exe"
    )
endif()

# ---------------------------------------------------------------------------
# Annual data and SearchEngine service archive helper
# ---------------------------------------------------------------------------
if(BUILD_ARCHIVE_TOOL)
    if(NOT WIN32)
        message(FATAL_ERROR "SearchEngineArchive currently supports Windows only")
    endif()

    add_executable(
            SearchEngineArchive
            tools/archive/main.cpp
            tools/archive/ArchiveCore.cpp
            tools/archive/ServiceArchive.cpp
            tools/archive/InteractivePaths.cpp
            src/Backup/FileHash.cpp
            src/Backup/SQLiteBackup.cpp
            src/MyUtils/Encoding.cpp
            lib/backup_sqlite/sqlite3.c
    )
    target_include_directories(
            SearchEngineArchive
            PRIVATE
            "${PROJECT_SOURCE_DIR}/lib"
            "${PROJECT_SOURCE_DIR}/lib/backup_sqlite"
            "${PROJECT_SOURCE_DIR}/src"
            "${PROJECT_SOURCE_DIR}/tools/archive"
    )
    target_link_libraries(
            SearchEngineArchive
            PRIVATE
            nlohmann_json::nlohmann_json
            advapi32
            shell32
            ole32
            uuid
    )
    target_compile_definitions(
            SearchEngineArchive
            PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )
    if(MSVC)
        set_source_files_properties(
                lib/backup_sqlite/sqlite3.c
                PROPERTIES LANGUAGE C
        )
    endif()
    searchengine_configure_target(SearchEngineArchive)
    searchengine_target_win32_versioninfo(
            SearchEngineArchive
            PRODUCT_NAME "SearchEngineService"
            ORIGINAL_FILENAME "SearchEngineArchive.exe"
    )
endif()

# ---------------------------------------------------------------------------
# Disposable full-data stand for archive/indexing E2E tests
# ---------------------------------------------------------------------------
if(BUILD_ARCHIVE_E2E_STAND_TOOL)
    if(NOT WIN32)
        message(FATAL_ERROR "SearchEngineArchiveE2EStand currently supports Windows only")
    endif()

    add_executable(
            SearchEngineArchiveE2EStand
            tools/archive_e2e_stand/main.cpp
            tools/archive_e2e_stand/StandBuilder.cpp
            src/Backup/FileHash.cpp
            src/MyUtils/Encoding.cpp
            lib/backup_sqlite/sqlite3.c
    )
    target_include_directories(
            SearchEngineArchiveE2EStand
            PRIVATE
            "${PROJECT_SOURCE_DIR}/lib"
            "${PROJECT_SOURCE_DIR}/lib/backup_sqlite"
            "${PROJECT_SOURCE_DIR}/src"
            "${PROJECT_SOURCE_DIR}/tools/archive_e2e_stand"
    )
    target_link_libraries(
            SearchEngineArchiveE2EStand
            PRIVATE
            nlohmann_json::nlohmann_json
    )
    target_compile_definitions(
            SearchEngineArchiveE2EStand
            PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )
    if(MSVC)
        set_source_files_properties(
                lib/backup_sqlite/sqlite3.c
                PROPERTIES LANGUAGE C
        )
    endif()
    searchengine_configure_target(SearchEngineArchiveE2EStand)
    searchengine_target_win32_versioninfo(
            SearchEngineArchiveE2EStand
            PRODUCT_NAME "SearchEngineArchiveE2EStand"
            ORIGINAL_FILENAME "SearchEngineArchiveE2EStand.exe"
    )
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

if(BUILD_SEARCHENGINE OR BUILD_TESTS OR BUILD_ZAGEDITOR OR BUILD_BACKUP_SERVICE)
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

if(BUILD_SEARCHENGINE OR BUILD_TESTS)
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
if(BUILD_SEARCHENGINE OR BUILD_TOKEN_ISSUER)
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
            SearchEngine::utf8proc
    )
    # Prefer the architecture-specific Shining Light VC import libraries.
    # FindOpenSSL can retain a Release cache entry from another installation
    # (for example, the ARM64 prefix in C:/Program Files/OpenSSL) even when
    # SEARCHENGINE_OPENSSL_ROOT points at OpenSSL-Win64.
    if(
            WIN32
            AND SEARCHENGINE_OPENSSL_ROOT
            AND EXISTS "${SEARCHENGINE_OPENSSL_ROOT}/lib/VC/${SEARCHENGINE_ARCH}/MD/libcrypto.lib"
            AND EXISTS "${SEARCHENGINE_OPENSSL_ROOT}/lib/VC/${SEARCHENGINE_ARCH}/MD/libssl.lib"
    )
        target_include_directories(
                SearchEngine
                PRIVATE
                "${SEARCHENGINE_OPENSSL_ROOT}/include"
        )
        target_link_libraries(
                SearchEngine
                PRIVATE
                "$<$<CONFIG:Debug>:${SEARCHENGINE_OPENSSL_ROOT}/lib/VC/${SEARCHENGINE_ARCH}/MDd/libcrypto.lib>"
                "$<$<CONFIG:Debug>:${SEARCHENGINE_OPENSSL_ROOT}/lib/VC/${SEARCHENGINE_ARCH}/MDd/libssl.lib>"
                "$<$<NOT:$<CONFIG:Debug>>:${SEARCHENGINE_OPENSSL_ROOT}/lib/VC/${SEARCHENGINE_ARCH}/MD/libcrypto.lib>"
                "$<$<NOT:$<CONFIG:Debug>>:${SEARCHENGINE_OPENSSL_ROOT}/lib/VC/${SEARCHENGINE_ARCH}/MD/libssl.lib>"
        )
    else()
        target_link_libraries(
                SearchEngine
                PRIVATE
                OpenSSL::Crypto
                OpenSSL::SSL
        )
    endif()
    if(WIN32)
        target_link_libraries(SearchEngine PRIVATE ws2_32 advapi32)
    endif()
    searchengine_configure_target(SearchEngine)
    target_compile_options(
            SearchEngine
            PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/MP>
    )
    searchengine_target_win32_versioninfo(
            SearchEngine
            PRODUCT_NAME "SearchEngineService"
            ORIGINAL_FILENAME "SearchEngine.exe"
    )

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

# Portable package + optional Drive publish after Release builds of the
# service targets. Packagable presets:
#   windows-x64      → x64
#   windows-x86      → x86-modern (VS2022/v143)
#   windows7-x86     → x86 (Win7/v142 portable)
get_filename_component(_searchengine_package_preset "${CMAKE_BINARY_DIR}" NAME)
set(_searchengine_package_arch "")
if(_searchengine_package_preset STREQUAL "windows-x64")
    set(_searchengine_package_arch "x64")
elseif(_searchengine_package_preset STREQUAL "windows-x86")
    set(_searchengine_package_arch "x86-modern")
elseif(_searchengine_package_preset STREQUAL "windows7-x86")
    set(_searchengine_package_arch "x86")
endif()
if(NOT _powershell_executable)
    find_program(_powershell_executable NAMES pwsh powershell)
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
            src/Backup/BackupPathFilter.cpp
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
    searchengine_target_win32_versioninfo(
            BackupService
            PRODUCT_NAME "BackupService"
            ORIGINAL_FILENAME "BackupService.exe"
    )
endif()

# ---------------------------------------------------------------------------
# AuthDbTool
# ---------------------------------------------------------------------------
if(BUILD_AUTH_DB_TOOL)
    add_executable(
            AuthDbTool
            tools/auth/main.cpp
            tools/auth/TokenLoader.cpp
            src/Auth/IssuerPublicKeyPath.cpp
            src/Auth/RsaIdentitySignatureVerifier.cpp
            src/Auth/AuthClientStore.cpp
            src/SQLite/sqlite3.c
    )
    target_include_directories(
            AuthDbTool
            PRIVATE
            "${PROJECT_SOURCE_DIR}/src"
            "${PROJECT_SOURCE_DIR}/tools/auth"
    )
    target_link_libraries(
            AuthDbTool
            PRIVATE
            nlohmann_json::nlohmann_json
    )
    if(MSVC)
        # sqlite3.c is C; keep C++20 for the tool sources.
        set_source_files_properties(
                src/SQLite/sqlite3.c
                PROPERTIES LANGUAGE C
        )
    endif()
    target_link_libraries(AuthDbTool PRIVATE OpenSSL::Crypto)
    searchengine_configure_target(AuthDbTool)
endif()

# ---------------------------------------------------------------------------
# SearchClientTokenIssuer (USB/computer auth token issuance; Windows-only)
# ---------------------------------------------------------------------------
if(BUILD_TOKEN_ISSUER)
    if(NOT WIN32)
        message(FATAL_ERROR "SearchClientTokenIssuer currently supports Windows only")
    endif()

    add_executable(
            SearchClientTokenIssuer
            tools/token_issuer/main.cpp
            tools/auth/TokenLoader.cpp
            tools/token_issuer/ComputerTokenPath.cpp
            tools/token_issuer/VolumeSerial.cpp
            tools/token_issuer/ComputerIdentity.cpp
            tools/token_issuer/TokenAscii.cpp
            tools/token_issuer/TokenDefaults.cpp
            tools/token_issuer/TokenDocument.cpp
            tools/token_issuer/CryptoStub.cpp
    )
    target_include_directories(
            SearchClientTokenIssuer
            PRIVATE
            "${PROJECT_SOURCE_DIR}/tools/token_issuer"
            "${PROJECT_SOURCE_DIR}/tools/auth"
            "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
            SearchClientTokenIssuer
            PRIVATE
            nlohmann_json::nlohmann_json
            ole32
            oleaut32
            wbemuuid
            comdlg32
    )
    # Prefer arch-matched OpenSSL VC MD libs. Stock FindOpenSSL may cache a
    # mismatched Release import lib (e.g. arm64) from another OpenSSL prefix.
    if(
            WIN32
            AND SEARCHENGINE_OPENSSL_ROOT
            AND EXISTS "${SEARCHENGINE_OPENSSL_ROOT}/lib/VC/${SEARCHENGINE_ARCH}/MD/libcrypto.lib"
    )
        target_include_directories(
                SearchClientTokenIssuer
                PRIVATE
                "${SEARCHENGINE_OPENSSL_ROOT}/include"
        )
        target_link_libraries(
                SearchClientTokenIssuer
                PRIVATE
                "$<$<CONFIG:Debug>:${SEARCHENGINE_OPENSSL_ROOT}/lib/VC/${SEARCHENGINE_ARCH}/MDd/libcrypto.lib>"
                "$<$<NOT:$<CONFIG:Debug>>:${SEARCHENGINE_OPENSSL_ROOT}/lib/VC/${SEARCHENGINE_ARCH}/MD/libcrypto.lib>"
        )
    else()
        target_link_libraries(
                SearchClientTokenIssuer
                PRIVATE
                OpenSSL::Crypto
        )
    endif()
    target_compile_definitions(
            SearchClientTokenIssuer
            PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )
    searchengine_configure_target(SearchClientTokenIssuer)
    add_custom_command(
            TARGET SearchClientTokenIssuer
            POST_BUILD
            COMMAND
            "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:SearchClientTokenIssuer>/data"
            COMMAND
            "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/tools/token_issuer/data/searchclient-auth-token.defaults.json"
            "$<TARGET_FILE_DIR:SearchClientTokenIssuer>/searchclient-auth-token.defaults.json"
            COMMAND
            "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/tools/token_issuer/data/searchclient-auth-token.defaults.json"
            "$<TARGET_FILE_DIR:SearchClientTokenIssuer>/data/searchclient-auth-token.defaults.json"
            COMMENT
            "Copy SearchClientTokenIssuer defaults JSON beside executable"
            VERBATIM
    )
endif()

if(
        WIN32
        AND SEARCHENGINE_PACKAGE_ON_RELEASE_BUILD
        AND _searchengine_package_arch
        AND _powershell_executable
)
    # Bump app-version before compile (not in POST_BUILD): PE VERSIONINFO must
    # embed the new patch. Build-*Package sets SEARCHENGINE_VERSION_BUMP_MODE=skip
    # and usually configures with PACKAGE_ON_RELEASE_BUILD=OFF to avoid a second bump.
    if(TARGET SearchEngine)
        if(TARGET SearchEngineConfig)
            searchengine_attach_release_version_bump(
                    SearchEngineService
                    SearchEngineConfig
                    SearchEngine
            )
        else()
            searchengine_attach_release_version_bump(
                    SearchEngineService
                    SearchEngine
            )
        endif()
    endif()
    if(TARGET SearchEngineArchive)
        searchengine_attach_release_version_bump(
                SearchEngineService
                SearchEngineArchive
        )
    endif()
    if(TARGET BackupService)
        searchengine_attach_release_version_bump(BackupService BackupService)
    endif()

    if(TARGET SearchEngine)
        if(TARGET SearchEngineConfig)
            add_dependencies(SearchEngine SearchEngineConfig)
        endif()
        # Portable package also ships AuthDbTool / TokenIssuer; build before PostBuild.
        if(TARGET AuthDbTool)
            add_dependencies(SearchEngine AuthDbTool)
        endif()
        if(TARGET SearchClientTokenIssuer)
            add_dependencies(SearchEngine SearchClientTokenIssuer)
        endif()
        if(TARGET SearchEngineArchive)
            add_dependencies(SearchEngine SearchEngineArchive)
        endif()
        add_custom_command(
                TARGET SearchEngine
                POST_BUILD
                COMMAND
                "${_powershell_executable}"
                -NoProfile
                -ExecutionPolicy Bypass
                -File
                "${CMAKE_SOURCE_DIR}/scripts/PostBuild-SearchEngineServicePackage.ps1"
                -Architecture
                "${_searchengine_package_arch}"
                -BuildDirectory
                "$<TARGET_FILE_DIR:SearchEngine>"
                -Configuration
                "$<CONFIG>"
                VERBATIM
                COMMENT
                "Packaging SearchEngineService ${_searchengine_package_arch} and publishing when cloud root is configured"
        )
    endif()
    if(TARGET BackupService)
        add_custom_command(
                TARGET BackupService
                POST_BUILD
                COMMAND
                "${_powershell_executable}"
                -NoProfile
                -ExecutionPolicy Bypass
                -File
                "${CMAKE_SOURCE_DIR}/scripts/PostBuild-BackupServicePackage.ps1"
                -Architecture
                "${_searchengine_package_arch}"
                -BuildDirectory
                "$<TARGET_FILE_DIR:BackupService>"
                -Configuration
                "$<CONFIG>"
                VERBATIM
                COMMENT
                "Packaging BackupService ${_searchengine_package_arch} and publishing when cloud root is configured"
        )
    endif()
elseif(
        WIN32
        AND SEARCHENGINE_PACKAGE_ON_RELEASE_BUILD
        AND _searchengine_package_arch
        AND NOT _powershell_executable
)
    message(
            WARNING
            "PowerShell was not found; SearchEngine/BackupService/ZagEditor/BackupRestore Release package PostBuild is disabled"
    )
endif()

# ---------------------------------------------------------------------------
# BackupRestore (mirror_history restore CLI + core)
# ---------------------------------------------------------------------------
if(BUILD_BACKUP_RESTORE)
    add_library(
            BackupRestoreCore
            STATIC
            src/Backup/FileHash.cpp
            src/Backup/Restore/MirrorHistoryStore.cpp
            src/Backup/Restore/MirrorHistoryRestore.cpp
            src/MyUtils/Encoding.cpp
    )
    target_include_directories(
            BackupRestoreCore
            PUBLIC
            "${PROJECT_SOURCE_DIR}/src"
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/lib"
    )
    target_link_libraries(
            BackupRestoreCore
            PUBLIC
            nlohmann_json::nlohmann_json
    )
    searchengine_configure_target(BackupRestoreCore)

    add_executable(
            BackupRestore
            tools/backup_restore/main.cpp
    )
    target_include_directories(
            BackupRestore
            PRIVATE
            "${PROJECT_SOURCE_DIR}/tools/backup_restore"
            "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
            BackupRestore
            PRIVATE
            BackupRestoreCore
    )
    searchengine_configure_target(BackupRestore)
    searchengine_target_win32_versioninfo(
            BackupRestore
            PRODUCT_NAME "BackupRestore"
            ORIGINAL_FILENAME "BackupRestore.exe"
    )

    if(
            WIN32
            AND SEARCHENGINE_PACKAGE_ON_RELEASE_BUILD
            AND _searchengine_package_arch
            AND _powershell_executable
    )
        searchengine_attach_release_version_bump(BackupRestore BackupRestore)
        add_custom_command(
                TARGET BackupRestore
                POST_BUILD
                COMMAND
                "${_powershell_executable}"
                -NoProfile
                -ExecutionPolicy Bypass
                -File
                "${CMAKE_SOURCE_DIR}/scripts/PostBuild-BackupRestorePackage.ps1"
                -Architecture
                "${_searchengine_package_arch}"
                -BuildDirectory
                "$<TARGET_FILE_DIR:BackupRestore>"
                -Configuration
                "$<CONFIG>"
                VERBATIM
                COMMENT
                "Packaging BackupRestore ${_searchengine_package_arch} and publishing when cloud root is configured"
        )
    endif()
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
    searchengine_target_win32_versioninfo(
            ZagEditor
            PRODUCT_NAME "ZagEditor"
            ORIGINAL_FILENAME "ZagEditor.exe"
    )

    if(
            WIN32
            AND SEARCHENGINE_PACKAGE_ON_RELEASE_BUILD
            AND _searchengine_package_arch
            AND _powershell_executable
    )
        searchengine_attach_release_version_bump(ZagEditor ZagEditor)
        add_custom_command(
                TARGET ZagEditor
                POST_BUILD
                COMMAND
                "${_powershell_executable}"
                -NoProfile
                -ExecutionPolicy Bypass
                -File
                "${CMAKE_SOURCE_DIR}/scripts/PostBuild-ZagEditorPackage.ps1"
                -Architecture
                "${_searchengine_package_arch}"
                -BuildDirectory
                "$<TARGET_FILE_DIR:ZagEditor>"
                -Configuration
                "$<CONFIG>"
                VERBATIM
                COMMENT
                "Packaging ZagEditor ${_searchengine_package_arch} and publishing when cloud root is configured"
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

# Interactive offline access setup; no additional scripting runtime is needed.
if(WIN32 AND BUILD_TOKEN_ISSUER AND BUILD_AUTH_DB_TOOL)
    add_library(SearchEngineAccessCore STATIC
        tools/access_setup/AccessBundle.cpp
        tools/auth/TokenLoader.cpp
        tools/token_issuer/TokenDocument.cpp
        tools/token_issuer/TokenAscii.cpp
        tools/token_issuer/CryptoStub.cpp
        src/Auth/AuthClientStore.cpp
        src/Auth/IssuerPublicKeyPath.cpp
        src/Auth/RsaIdentitySignatureVerifier.cpp
        src/SQLite/sqlite3.c)
    target_include_directories(SearchEngineAccessCore PUBLIC
        "${PROJECT_SOURCE_DIR}/tools/access_setup"
        "${PROJECT_SOURCE_DIR}/tools/auth"
        "${PROJECT_SOURCE_DIR}/tools/token_issuer"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(SearchEngineAccessCore PUBLIC nlohmann_json::nlohmann_json OpenSSL::Crypto)
    target_compile_definitions(SearchEngineAccessCore PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    searchengine_configure_target(SearchEngineAccessCore)
    add_executable(SearchEngineAccessSetup
        tools/access_setup/main.cpp
        tools/token_issuer/ComputerIdentity.cpp
        tools/token_issuer/ComputerTokenPath.cpp)
    target_link_libraries(SearchEngineAccessSetup PRIVATE SearchEngineAccessCore
        comdlg32 shell32 advapi32 ole32 oleaut32 wbemuuid)
    searchengine_configure_target(SearchEngineAccessSetup)
    searchengine_target_win32_versioninfo(SearchEngineAccessSetup
        PRODUCT_NAME "SearchEngineService" ORIGINAL_FILENAME "SearchEngineAccessSetup.exe")
    add_dependencies(SearchEngineAccessSetup SearchClientTokenIssuer)
    if(TARGET SearchEngine)
        add_dependencies(SearchEngine SearchEngineAccessSetup)
    endif()
    if(BUILD_TESTS)
        add_executable(AccessSetup_test tests/auth/AccessSetupTests.cpp src/Commands/Auth/AuthenticateCmd.cpp)
        target_link_libraries(AccessSetup_test PRIVATE SearchEngineAccessCore ole32)
        searchengine_configure_target(AccessSetup_test)
        add_test(NAME AccessSetup_test COMMAND AccessSetup_test)
    endif()
endif()

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
    list(
            FILTER test_sources
            EXCLUDE REGEX "/tests/index/InvertedIndexSQLiteIntegrationTests\\.cpp$"
    )
    list(
            FILTER test_sources
            EXCLUDE REGEX "/tests/archive/ArchiveE2EStandTests\\.cpp$"
    )
    list(FILTER test_sources EXCLUDE REGEX "/tests/auth/AccessSetupTests\\.cpp$")
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
              "${PROJECT_SOURCE_DIR}/src/Backup/BackupPathFilter.cpp"
            "${PROJECT_SOURCE_DIR}/src/Backup/FileHash.cpp"
            "${PROJECT_SOURCE_DIR}/src/Backup/MirrorHistory.cpp"
            "${PROJECT_SOURCE_DIR}/src/Backup/Restore/MirrorHistoryStore.cpp"
            "${PROJECT_SOURCE_DIR}/src/Backup/Restore/MirrorHistoryRestore.cpp"
            "${PROJECT_SOURCE_DIR}/src/Backup/SQLiteBackup.cpp"
            "${PROJECT_SOURCE_DIR}/tools/archive/ArchiveCore.cpp"
            "${PROJECT_SOURCE_DIR}/tools/archive/ServiceArchive.cpp"
            "${PROJECT_SOURCE_DIR}/tools/archive/InteractivePaths.cpp"
            "${PROJECT_SOURCE_DIR}/lib/backup_sqlite/sqlite3.c"
            "${PROJECT_SOURCE_DIR}/src/Commands/GetFile/GetFileCmd.cpp"
            "${PROJECT_SOURCE_DIR}/src/Commands/GetAttachments/GetAttachmentsCmd.cpp"
            "${PROJECT_SOURCE_DIR}/src/Application/SearchEngineOptions.cpp"
            "${PROJECT_SOURCE_DIR}/src/Commands/Auth/AuthenticateCmd.cpp"
            "${PROJECT_SOURCE_DIR}/src/Auth/AuthClientStore.cpp"
            "${PROJECT_SOURCE_DIR}/src/Auth/IssuerPublicKeyPath.cpp"
            "${PROJECT_SOURCE_DIR}/tools/auth/TokenLoader.cpp"
            "${PROJECT_SOURCE_DIR}/tools/token_issuer/TokenAscii.cpp"
            "${PROJECT_SOURCE_DIR}/tools/token_issuer/ComputerTokenPath.cpp"
            "${PROJECT_SOURCE_DIR}/tools/token_issuer/TokenDocument.cpp"
            "${PROJECT_SOURCE_DIR}/src/Commands/GetAttachments/AttachmentPackage.cpp"
            "${PROJECT_SOURCE_DIR}/src/Commands/SaveFile/FileData.cpp"
            "${PROJECT_SOURCE_DIR}/src/Commands/SaveFile/SaveFileCmd.cpp"
            "${PROJECT_SOURCE_DIR}/src/JSON/ConverterJSON.cpp"
            "${PROJECT_SOURCE_DIR}/src/Commands/ServiceCommands/PingCmd.cpp"
            "${PROJECT_SOURCE_DIR}/src/Commands/GetTelegaAttachments/GetTelegaAttachments.cpp"
            "${PROJECT_SOURCE_DIR}/src/Commands/TelegramFiles/TelegramFileResolver.cpp"
            "${PROJECT_SOURCE_DIR}/src/Commands/TelegramFiles/PdtvFileResolver.cpp"
            "${PROJECT_SOURCE_DIR}/src/Commands/StreamingUpload/StreamingUpload.cpp"
            "${PROJECT_SOURCE_DIR}/src/Commands/GetJsonTelega/Telega.cpp"
            "${PROJECT_SOURCE_DIR}/src/Commands/GetJsonTelega/AutoPadSource.cpp"
            "${PROJECT_SOURCE_DIR}/src/Commands/GetJsonTelega/GetJsonTelegaCmd.cpp"
            "${PROJECT_SOURCE_DIR}/src/Commands/GetIshTelegaPdtv/GetIshTelegaPdtvCommand.cpp"
            "${PROJECT_SOURCE_DIR}/src/Commands/GetTelegaWay/TelegaWay.cpp"
            "${PROJECT_SOURCE_DIR}/src/Commands/GetTelegaWay/GetTelegaWayCmd.cpp"
            "${PROJECT_SOURCE_DIR}/src/MyUtils/OEMCase.cpp"
            "${PROJECT_SOURCE_DIR}/src/MyUtils/Encoding.cpp"
            "${PROJECT_SOURCE_DIR}/src/MyUtils/FileExtensionContract.cpp"
            "${PROJECT_SOURCE_DIR}/src/MyUtils/FileScanner.cpp"
            "${PROJECT_SOURCE_DIR}/src/MyUtils/LogFile.cpp"
            "${PROJECT_SOURCE_DIR}/src/MyUtils/PathExclusion.cpp"
            "${PROJECT_SOURCE_DIR}/src/SQLite/SQLiteConnectionManager.cpp"
            "${PROJECT_SOURCE_DIR}/src/SQLite/mySQLite.cpp"
            "${PROJECT_SOURCE_DIR}/src/Utils/portable_binary_iarchive.cpp"
            "${PROJECT_SOURCE_DIR}/src/Utils/portable_binary_oarchive.cpp"
            "${PROJECT_SOURCE_DIR}/src/Index/Batch/BatchIndexBuilder.cpp"
            "${PROJECT_SOURCE_DIR}/src/Index/DocPaths.cpp"
            "${PROJECT_SOURCE_DIR}/src/Index/OEMFastTokenizer.cpp"
            "${PROJECT_SOURCE_DIR}/src/Index/simd_tokenizer.cpp"
            "${PROJECT_SOURCE_DIR}/src/scheduler/AbstractScheduledTask.cpp"
            "${PROJECT_SOURCE_DIR}/src/scheduler/BackupTask.cpp"
            "${PROJECT_SOURCE_DIR}/src/SearchServer/QueryWordMatch.cpp"
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
            "${PROJECT_SOURCE_DIR}/tools/auth"
            "${PROJECT_SOURCE_DIR}/tools/token_issuer"
            "${PROJECT_SOURCE_DIR}/tools/archive"
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
            SearchEngine::utf8proc
    )
    if(WIN32)
        target_link_libraries(
                SearchEngine_test
                PRIVATE
                ws2_32
                user32
                advapi32
                shell32
                ole32
                uuid
        )
    endif()
    searchengine_configure_target(SearchEngine_test)
    add_test(NAME SearchEngine_test COMMAND SearchEngine_test)

    if(WIN32 AND TARGET SearchClientTokenIssuer AND TARGET AuthDbTool)
        add_test(
                NAME TokenRequestFlow_test
                COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
                -File "${PROJECT_SOURCE_DIR}/scripts/tests/TokenRequestFlow.Tests.ps1"
                -IssuerExe "$<TARGET_FILE:SearchClientTokenIssuer>"
                -AuthDbExe "$<TARGET_FILE:AuthDbTool>"
                -OpenSslExe "${SEARCHENGINE_OPENSSL_ROOT}/bin/openssl.exe"
                -WorkRoot "${CMAKE_CURRENT_BINARY_DIR}/token-request-tests"
        )
    endif()

    add_executable(
            ArchiveE2EStand_test
            tests/archive/ArchiveE2EStandTests.cpp
            tools/archive_e2e_stand/StandBuilder.cpp
            tools/archive/ArchiveCore.cpp
            tools/archive/ServiceArchive.cpp
            src/Backup/FileHash.cpp
            src/Backup/SQLiteBackup.cpp
            src/MyUtils/Encoding.cpp
            lib/backup_sqlite/sqlite3.c
    )
    target_include_directories(
            ArchiveE2EStand_test
            PRIVATE
            "${PROJECT_SOURCE_DIR}/lib"
            "${PROJECT_SOURCE_DIR}/lib/backup_sqlite"
            "${PROJECT_SOURCE_DIR}/src"
            "${PROJECT_SOURCE_DIR}/tools/archive"
            "${PROJECT_SOURCE_DIR}/tools/archive_e2e_stand"
    )
    target_link_libraries(
            ArchiveE2EStand_test
            PRIVATE
            GTest::gtest
            GTest::gtest_main
            nlohmann_json::nlohmann_json
            advapi32
            shell32
            ole32
            uuid
    )
    target_compile_definitions(
            ArchiveE2EStand_test
            PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )
    searchengine_configure_target(ArchiveE2EStand_test)
    add_test(NAME ArchiveE2EStand_test COMMAND ArchiveE2EStand_test)

    add_executable(
            InvertedIndexSQLiteIntegration_test
            tests/index/InvertedIndexSQLiteIntegrationTests.cpp
            src/Index/InvertedIndex.cpp
            src/Index/BoostIndexSerializer.cpp
            src/Index/SQLiteIndexSerializer.cpp
            src/Index/Batch/BatchIndexBuilder.cpp
            src/Index/DocumentCatalog.cpp
            src/Index/DocPaths.cpp
            src/Index/OEMFastTokenizer.cpp
            src/Index/PostingList.cpp
            src/Index/simd_tokenizer.cpp
            src/Index/WordID.cpp
            src/MyUtils/OEMCase.cpp
            src/MyUtils/Encoding.cpp
            src/MyUtils/LogFile.cpp
            src/SQLite/sqlite3.c
    )
    target_include_directories(
            InvertedIndexSQLiteIntegration_test
            PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/lib"
            "${PROJECT_SOURCE_DIR}/src"
            "${PROJECT_SOURCE_DIR}/src/SQLite"
    )
    target_link_libraries(
            InvertedIndexSQLiteIntegration_test
            PRIVATE
            GTest::gtest
            GTest::gtest_main
            Boost::headers
            Boost::system
            Boost::filesystem
            Boost::serialization
            SearchEngine::utf8proc
    )
    if(WIN32)
        target_link_libraries(
                InvertedIndexSQLiteIntegration_test
                PRIVATE
                ws2_32
                psapi
        )
    endif()
    searchengine_configure_target(InvertedIndexSQLiteIntegration_test)
    add_test(
            NAME InvertedIndexSQLiteIntegration_test
            COMMAND InvertedIndexSQLiteIntegration_test
    )
    if(TARGET SearchEngineConfig)
        add_test(
                NAME SearchEngineConfigDocumentCatalog_test
                COMMAND
                "${CMAKE_COMMAND}"
                "-DCONFIG_EXE=$<TARGET_FILE:SearchEngineConfig>"
                "-DTEMPLATE=${PROJECT_SOURCE_DIR}/deployment/SearchEngineServicePortable/source-data/Settings.json"
                "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/config-document-catalog-test"
                -P
                "${PROJECT_SOURCE_DIR}/tests/config/SearchEngineConfigDocumentCatalog.cmake"
        )
        add_test(
                NAME SearchEngineConfigAutoPadPaths_test
                COMMAND
                "${CMAKE_COMMAND}"
                "-DCONFIG_EXE=$<TARGET_FILE:SearchEngineConfig>"
                "-DTEMPLATE=${PROJECT_SOURCE_DIR}/deployment/SearchEngineServicePortable/source-data/Settings.json"
                "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/config-autopad-paths-test"
                -P
                "${PROJECT_SOURCE_DIR}/tests/config/SearchEngineConfigAutoPadPaths.cmake"
        )
    endif()
endif()

# SVC-001 config-tool tests: registered whenever SearchEngineConfig is built,
# independently of BUILD_TESTS (which gates GoogleTest unit tests only).
if(TARGET SearchEngineConfig)
    enable_testing()
    add_test(
            NAME SearchEngineConfigLocalMachine_test
            COMMAND
            "${CMAKE_COMMAND}"
            "-DCONFIG_EXE=$<TARGET_FILE:SearchEngineConfig>"
            "-DTEMPLATE=${PROJECT_SOURCE_DIR}/deployment/SearchEngineServicePortable/source-data/Settings.json"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/config-local-machine-test"
            -P
            "${PROJECT_SOURCE_DIR}/tests/config/SearchEngineConfigLocalMachine.cmake"
    )
    add_test(
            NAME SearchEngineConfigValidateSVC001Fields_test
            COMMAND
            "${CMAKE_COMMAND}"
            "-DCONFIG_EXE=$<TARGET_FILE:SearchEngineConfig>"
            "-DTEMPLATE=${PROJECT_SOURCE_DIR}/deployment/SearchEngineServicePortable/source-data/Settings.json"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/config-validate-svc001-test"
            -P
            "${PROJECT_SOURCE_DIR}/tests/config/SearchEngineConfigValidateSVC001Fields.cmake"
    )
    add_test(
            NAME SearchEngineConfigSettingsTransaction_test
            COMMAND
            "${CMAKE_COMMAND}"
            "-DCONFIG_EXE=$<TARGET_FILE:SearchEngineConfig>"
            "-DTEMPLATE=${PROJECT_SOURCE_DIR}/deployment/SearchEngineServicePortable/source-data/Settings.json"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/config-settings-transaction-test"
            -P
            "${PROJECT_SOURCE_DIR}/tests/config/SearchEngineConfigSettingsTransaction.cmake"
    )
    add_test(
            NAME SearchEngineConfigFormatJson_test
            COMMAND
            "${CMAKE_COMMAND}"
            "-DCONFIG_EXE=$<TARGET_FILE:SearchEngineConfig>"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/config-format-json-test"
            -P
            "${PROJECT_SOURCE_DIR}/tests/config/SearchEngineConfigFormatJson.cmake"
    )
    add_test(
            NAME SearchEngineConfigScriptMessages_test
            COMMAND
            "${CMAKE_COMMAND}"
            "-DCONFIG_EXE=$<TARGET_FILE:SearchEngineConfig>"
            -P
            "${PROJECT_SOURCE_DIR}/tests/config/SearchEngineConfigScriptMessages.cmake"
    )
    add_test(
            NAME SearchEngineConfigBlock1Legacy_test
            COMMAND
            "${CMAKE_COMMAND}"
            "-DCONFIG_EXE=$<TARGET_FILE:SearchEngineConfig>"
            "-DTEMPLATE=${PROJECT_SOURCE_DIR}/deployment/SearchEngineServicePortable/source-data/Settings.json"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/config-block1-legacy-test"
            -P
            "${PROJECT_SOURCE_DIR}/tests/config/SearchEngineConfigBlock1Legacy.cmake"
    )
    add_test(
            NAME SearchEngineConfigBlock2IndexRoots_test
            COMMAND
            "${CMAKE_COMMAND}"
            "-DCONFIG_EXE=$<TARGET_FILE:SearchEngineConfig>"
            "-DTEMPLATE=${PROJECT_SOURCE_DIR}/deployment/SearchEngineServicePortable/source-data/Settings.json"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/config-block2-index-roots-test"
            -P
            "${PROJECT_SOURCE_DIR}/tests/config/SearchEngineConfigBlock2IndexRoots.cmake"
    )
    add_test(
            NAME SearchEngineConfigBlock2B_test
            COMMAND
            "${CMAKE_COMMAND}"
            "-DCONFIG_EXE=$<TARGET_FILE:SearchEngineConfig>"
            "-DTEMPLATE=${PROJECT_SOURCE_DIR}/deployment/SearchEngineServicePortable/source-data/Settings.json"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/config-block2b-test"
            -P
            "${PROJECT_SOURCE_DIR}/tests/config/SearchEngineConfigBlock2B.cmake"
    )
    find_program(
            SEARCHENGINE_POWERSHELL_EXECUTABLE
            NAMES pwsh.exe pwsh powershell.exe powershell
    )
    if(SEARCHENGINE_POWERSHELL_EXECUTABLE)
        add_test(
                NAME YearBasedReleaseSettings_test
                COMMAND
                "${SEARCHENGINE_POWERSHELL_EXECUTABLE}"
                -NoProfile
                -ExecutionPolicy Bypass
                -File
                "${PROJECT_SOURCE_DIR}/scripts/tests/YearBasedReleaseSettings.Tests.ps1"
                -HelperPath "$<TARGET_FILE:SearchEngineConfig>"
        )
        add_test(
                NAME SearchEngineSettingsMerge_test
                COMMAND
                "${SEARCHENGINE_POWERSHELL_EXECUTABLE}"
                -NoProfile
                -ExecutionPolicy Bypass
                -File
                "${PROJECT_SOURCE_DIR}/scripts/tests/SearchEngineSettingsMerge.Tests.ps1"
                -HelperPath "$<TARGET_FILE:SearchEngineConfig>"
        )
    endif()
endif()
