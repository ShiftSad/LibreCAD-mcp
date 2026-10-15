# librecad-mcp: in-app Remote API (local HTTP server used by the librecad-mcp
# MCP server). Included by the top-level CMakeLists.txt; the sources are built
# into librecad_lib with the rest of the application. Mirrors remote_api.pri.
#
# Each component directory has its own fragment:
#   core/core.cmake            always present
#   entities/entities.cmake    optional
#   documents/documents.cmake  optional
# A fragment appends to these lists (paths relative to the top-level source dir):
#   list(APPEND LC_REMOTE_API_SOURCES      librecad/src/remote_api/<dir>/<file>.cpp ... .h)
#   list(APPEND LC_REMOTE_API_INCLUDES     librecad/src/remote_api/<dir>)
#   list(APPEND LC_REMOTE_API_TEST_SOURCES librecad/src/remote_api/<dir>/tests/<file>.cpp)
#   list(APPEND LC_REMOTE_API_DEFINES      LC_REMOTE_API_HAS_<DIR>)    # entities/documents only
# and lists every file in its .pri twin as well.

# Build number of the LibreCAD-mcp fork, reported by /status as "mcp_build".
# Keep it equal to the value in remote_api.pri.
set(LC_MCP_RELEASE 1)
# The release tag of this build, v<LibreCAD version>-mcp.<build>: LC_VERSION
# without its label (2.2.2.6-alpha -> 2.2.2.6). Fixed here, never derived from
# git describe (qmake builds put that into LC_VERSION). "Copy MCP config" pins
# the server source to it; mcp/scripts/cut-release.sh checks it. Keep it equal
# to the value in remote_api.pri.
set(LC_MCP_TAG v2.2.2.6-mcp.${LC_MCP_RELEASE})

set(LC_REMOTE_API_SOURCES)
set(LC_REMOTE_API_INCLUDES)
set(LC_REMOTE_API_DEFINES LC_MCP_RELEASE=${LC_MCP_RELEASE})
set(LC_REMOTE_API_TEST_SOURCES)

# The optional includes below are invisible to the build system while their
# file is missing: re-run the configure step when a fragment appears or goes.
file(GLOB LC_REMOTE_API_FRAGMENTS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_LIST_DIR}/*/*.cmake")

include(${CMAKE_CURRENT_LIST_DIR}/core/core.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/entities/entities.cmake OPTIONAL)
include(${CMAKE_CURRENT_LIST_DIR}/documents/documents.cmake OPTIONAL)

# Only the controller reads the release tag and the server source (keeps the
# rest of librecad_lib from rebuilding when they change). LC_MCP_SOURCE_DIR is
# the mcp/ folder of this checkout: "Copy MCP config" runs the server from it
# while it exists (a development build), else from git at LC_MCP_TAG.
set_property(SOURCE librecad/src/remote_api/core/lc_remoteapicontroller.cpp APPEND PROPERTY COMPILE_DEFINITIONS
             LC_MCP_TAG=${LC_MCP_TAG} "LC_MCP_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}/mcp\"")
