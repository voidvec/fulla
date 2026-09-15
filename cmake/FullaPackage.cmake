# FullaPackage.cmake -- shared SDK packaging helper (fulla-sdk-refactor Task 28a).
#
# Provides fulla_package(), used by every libs/* package to emit a proper
# find_package()-consumable CMake config in BOTH:
#   - the install tree (lib/cmake/<pkg>/<pkg>Config.cmake), and
#   - the build tree   (${CMAKE_BINARY_DIR}/fulla-cmake/<pkg>/<pkg>Config.cmake)
#
# Before this task each package emitted only a bare install(EXPORT) whose file
# was mis-named <pkg>Config.cmake (it was really a *Targets* file): no
# @PACKAGE_INIT@, no find_dependency() of the transitive closure, and -- because
# no EXPORT_NAME was set -- the exported target came out as
# fulla::fulla-<pkg> instead of the canonical fulla::<alias> that
# in-tree code and design.md §5.5 use. All three gaps are closed here.
#
# The build-tree export lets examples/ and tests/ consume the whole stack via
# find_package() (CMAKE_PREFIX_PATH pointing at FULLA_BUILD_CMAKE_DIR)
# without an install step -- the mechanism behind the SdkSmoke build-and-test.

include_guard(GLOBAL)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Directory of THIS module. Captured once (CACHE INTERNAL so it is visible from
# every directory scope -- include_guard(GLOBAL) runs this body only in the
# first package's scope, and fulla_package() is later called from other
# subdirectories). Keeps the Config template path correct whether a package is
# configured in-tree or standalone (add_subdirectory from a foreign root, per
# the self-contained-bootstrap pattern the libs follow).
set(FULLA_PACKAGE_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}"
    CACHE INTERNAL "Directory containing FullaPackage.cmake + its template")

# Single shared root under the top build dir where every package writes its
# build-tree Config/Targets/ConfigVersion (<root>/<package>/...). A consumer
# adds this root to CMAKE_PREFIX_PATH to resolve the entire fulla closure
# (find_package searches <prefix>/<name>/).
if(NOT DEFINED FULLA_BUILD_CMAKE_DIR)
    set(FULLA_BUILD_CMAKE_DIR "${CMAKE_BINARY_DIR}/fulla-cmake"
        CACHE INTERNAL "Root dir for fulla build-tree package configs")
endif()

# fulla_package(
#     TARGET       <real-target-name>          # required, e.g. fulla-common
#     PACKAGE      <find_package-name>          # optional, defaults to TARGET
#     EXPORT_NAME  <fulla::<name> suffix>   # required, e.g. common / storage::memory
#     DEPENDENCIES <pkg> [<pkg> ...]            # find_dependency() closure (plain names)
# )
function(fulla_package)
    set(options)
    set(oneValueArgs TARGET PACKAGE EXPORT_NAME)
    set(multiValueArgs DEPENDENCIES)
    cmake_parse_arguments(AP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT AP_TARGET)
        message(FATAL_ERROR "fulla_package: TARGET is required")
    endif()
    if(NOT AP_PACKAGE)
        set(AP_PACKAGE "${AP_TARGET}")
    endif()
    if(NOT AP_EXPORT_NAME)
        message(FATAL_ERROR
            "fulla_package: EXPORT_NAME is required "
            "(the fulla:: alias suffix, e.g. common / storage::memory)")
    endif()

    set(_export_set "${AP_PACKAGE}Targets")
    set(_version "${FULLA_PROJECT_VERSION}")
    if(NOT _version)
        set(_version "1.3.1")
    endif()

    # Consumers get fulla::<export-name>, matching the in-tree ALIAS and
    # design.md's canonical names (without this, NAMESPACE fulla:: would
    # yield fulla::fulla-<pkg>).
    set_target_properties(${AP_TARGET} PROPERTIES EXPORT_NAME "${AP_EXPORT_NAME}")

    # --- install-tree target install ----------------------------------------
    install(TARGETS ${AP_TARGET}
        EXPORT ${_export_set}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    # find_dependency() block woven into the Config template (one per closure dep).
    set(FULLA_FIND_DEPENDENCY_BLOCK "")
    foreach(_dep IN LISTS AP_DEPENDENCIES)
        string(APPEND FULLA_FIND_DEPENDENCY_BLOCK "find_dependency(${_dep})\n")
    endforeach()
    set(FULLA_PKG_NAME "${AP_PACKAGE}")

    set(_template "${FULLA_PACKAGE_MODULE_DIR}/FullaPackageConfig.cmake.in")

    # --- install tree: Config + ConfigVersion + EXPORT ----------------------
    set(_install_cmakedir "${CMAKE_INSTALL_LIBDIR}/cmake/${AP_PACKAGE}")
    set(_install_stage "${CMAKE_CURRENT_BINARY_DIR}/fulla-package/install")
    configure_package_config_file(
        "${_template}"
        "${_install_stage}/${AP_PACKAGE}Config.cmake"
        INSTALL_DESTINATION "${_install_cmakedir}"
    )
    write_basic_package_version_file(
        "${_install_stage}/${AP_PACKAGE}ConfigVersion.cmake"
        VERSION "${_version}"
        COMPATIBILITY SameMajorVersion
    )
    install(EXPORT ${_export_set}
        FILE ${AP_PACKAGE}Targets.cmake
        NAMESPACE fulla::
        DESTINATION "${_install_cmakedir}"
    )
    install(FILES
        "${_install_stage}/${AP_PACKAGE}Config.cmake"
        "${_install_stage}/${AP_PACKAGE}ConfigVersion.cmake"
        DESTINATION "${_install_cmakedir}"
    )

    # --- build tree: export() + Config + ConfigVersion ----------------------
    # Consumed by examples/ + tests/ via find_package() with no install step.
    set(_build_cmakedir "${FULLA_BUILD_CMAKE_DIR}/${AP_PACKAGE}")
    export(EXPORT ${_export_set}
        FILE "${_build_cmakedir}/${AP_PACKAGE}Targets.cmake"
        NAMESPACE fulla::
    )
    configure_package_config_file(
        "${_template}"
        "${_build_cmakedir}/${AP_PACKAGE}Config.cmake"
        INSTALL_DESTINATION "${_build_cmakedir}"
        INSTALL_PREFIX "${_build_cmakedir}"
    )
    write_basic_package_version_file(
        "${_build_cmakedir}/${AP_PACKAGE}ConfigVersion.cmake"
        VERSION "${_version}"
        COMPATIBILITY SameMajorVersion
    )
endfunction()
