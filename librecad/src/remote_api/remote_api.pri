# librecad-mcp: in-app Remote API (local HTTP server used by the librecad-mcp
# MCP server). Included by src.pro. Mirrors remote_api.cmake.
#
# Each component directory has its own fragment: core/core.pri is always
# present; entities/entities.pri and documents/documents.pri are optional.
# A fragment does INCLUDEPATH += $$PWD, HEADERS += ..., SOURCES += ... and,
# for entities/documents, DEFINES += LC_REMOTE_API_HAS_<DIR>.

# Build number of the LibreCAD-mcp fork, reported by /status as "mcp_build".
# Keep it equal to the value in remote_api.cmake.
DEFINES += LC_MCP_RELEASE=1
# The release tag of this build (see remote_api.cmake); unquoted, the code
# stringifies it as it does LC_VERSION. Keep it equal to remote_api.cmake.
DEFINES += LC_MCP_TAG=v2.2.2.6-mcp.1

include($$PWD/core/core.pri)

exists($$PWD/entities/entities.pri) {
    include($$PWD/entities/entities.pri)
}

exists($$PWD/documents/documents.pri) {
    include($$PWD/documents/documents.pri)
}
