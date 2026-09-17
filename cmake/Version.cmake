# cmake/Version.cmake
# Single source of truth for the repo-wide version number.
# Referenced by root CMakeLists.txt and propagated to OAuth2Plugin / OAuth2Server
# sub-projects via ${FULLA_PROJECT_VERSION}.
#
# _Design: repo-structure-refactor §7.2_
# _Requirements: 9.2_

set(FULLA_PROJECT_VERSION_MAJOR 1)
set(FULLA_PROJECT_VERSION_MINOR 3)
set(FULLA_PROJECT_VERSION_PATCH 2)
set(FULLA_PROJECT_VERSION
    "${FULLA_PROJECT_VERSION_MAJOR}.${FULLA_PROJECT_VERSION_MINOR}.${FULLA_PROJECT_VERSION_PATCH}")
