# Attach generated Win32 VERSIONINFO resources produced by
# TOOLS/scripts/release/Sync-CmakeProjectVersion.ps1.
#
# Expected layout:
#   cmake/generated/<ProductName>/<TargetBase>_version.rc
#   cmake/generated/<ProductName>/version.h

function(searchengine_target_win32_versioninfo target_name)
    if(NOT WIN32)
        return()
    endif()

    set(options)
    set(oneValueArgs PRODUCT_NAME ORIGINAL_FILENAME)
    set(multiValueArgs)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ARG_PRODUCT_NAME)
        message(FATAL_ERROR "searchengine_target_win32_versioninfo(${target_name}): PRODUCT_NAME is required")
    endif()
    if(NOT ARG_ORIGINAL_FILENAME)
        message(FATAL_ERROR "searchengine_target_win32_versioninfo(${target_name}): ORIGINAL_FILENAME is required")
    endif()

    get_filename_component(_original_base "${ARG_ORIGINAL_FILENAME}" NAME_WE)
    set(_generated_dir "${PROJECT_SOURCE_DIR}/cmake/generated/${ARG_PRODUCT_NAME}")
    set(_rc_path "${_generated_dir}/${_original_base}_version.rc")
    set(_header_path "${_generated_dir}/version.h")

    if(NOT EXISTS "${_rc_path}")
        message(
            FATAL_ERROR
            "Missing generated VERSIONINFO resource for ${target_name}:\n"
            "  ${_rc_path}\n"
            "Run TOOLS/scripts/release/Sync-CmakeProjectVersion.ps1 for "
            "${ARG_PRODUCT_NAME} (or scripts/Build-*Package.ps1)."
        )
    endif()

    target_sources(${target_name} PRIVATE "${_rc_path}")
    if(EXISTS "${_header_path}")
        target_include_directories(${target_name} PRIVATE "${_generated_dir}")
    endif()

    # Rebuild resources when the canonical JSON changes.
    if(ARG_PRODUCT_NAME STREQUAL "SearchEngineService")
        set(_version_json "${PROJECT_SOURCE_DIR}/app-version.json")
    else()
        set(_version_json "${PROJECT_SOURCE_DIR}/app-version.${ARG_PRODUCT_NAME}.json")
    endif()
    if(EXISTS "${_version_json}")
        # Rebuild the .rc object when the canonical JSON changes. Do NOT add
        # CMAKE_CONFIGURE_DEPENDS here: Release bump runs during the build and
        # must not force a mid-build CMake reconfigure.
        set_source_files_properties(
            "${_rc_path}"
            PROPERTIES OBJECT_DEPENDS "${_version_json}"
        )
    endif()
endfunction()

# Attach a once-per-product Release version bump that runs before the target
# compiles. Only registered when SEARCHENGINE_PACKAGE_ON_RELEASE_BUILD is ON
# for a packagable preset (caller must pass the packaging gate).
function(searchengine_attach_release_version_bump product_name)
    if(NOT WIN32)
        return()
    endif()
    if(NOT SEARCHENGINE_PACKAGE_ON_RELEASE_BUILD)
        return()
    endif()
    if(NOT _searchengine_package_arch)
        return()
    endif()
    if(NOT _powershell_executable)
        return()
    endif()
    if(ARGC LESS 2)
        message(
            FATAL_ERROR
            "searchengine_attach_release_version_bump(${product_name}): "
            "at least one target name is required"
        )
    endif()

    set(_bump_target "searchengine_release_version_bump_${product_name}")
    if(NOT TARGET ${_bump_target})
        add_custom_target(
            ${_bump_target}
            COMMAND
            "${_powershell_executable}"
            -NoProfile
            -ExecutionPolicy Bypass
            -File
            "${PROJECT_SOURCE_DIR}/scripts/Ensure-ReleaseVersionBump.ps1"
            -ProjectRoot
            "${PROJECT_SOURCE_DIR}"
            -ProductName
            "${product_name}"
            -Configuration
            "$<CONFIG>"
            COMMENT
            "Bump ${product_name} app-version before Release compile (packagable preset)"
            VERBATIM
        )
        set_property(TARGET ${_bump_target} PROPERTY FOLDER "Versioning")
    endif()

    foreach(_target_name IN LISTS ARGN)
        if(NOT TARGET ${_target_name})
            message(
                FATAL_ERROR
                "searchengine_attach_release_version_bump: target "
                "'${_target_name}' does not exist"
            )
        endif()
        add_dependencies(${_target_name} ${_bump_target})
    endforeach()
endfunction()
