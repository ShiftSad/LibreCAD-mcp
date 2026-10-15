# librecad-mcp: Remote API documents component (formats, documents, I/O,
# drawing info, render, view, undo/redo, command line). Included by
# ../remote_api.cmake. Every source file listed here is also listed in
# documents.pri (the tests are CMake-only and run in librecad_remote_api_tests).

list(APPEND LC_REMOTE_API_SOURCES
     librecad/src/remote_api/documents/lc_remoteapiappaccess.cpp
     librecad/src/remote_api/documents/lc_remoteapiappaccess.h
     librecad/src/remote_api/documents/lc_remoteapidocumentsstorage.cpp
     librecad/src/remote_api/documents/lc_remoteapidocumentsstorage.h
     librecad/src/remote_api/documents/lc_remoteapiformats.cpp
     librecad/src/remote_api/documents/lc_remoteapiformats.h
     librecad/src/remote_api/documents/lc_remoteapipathpolicy.cpp
     librecad/src/remote_api/documents/lc_remoteapipathpolicy.h
     librecad/src/remote_api/documents/lc_remoteapirenderer.cpp
     librecad/src/remote_api/documents/lc_remoteapirenderer.h
     librecad/src/remote_api/documents/lc_remoteapiroutes_documents.cpp
)

list(APPEND LC_REMOTE_API_INCLUDES
     librecad/src/remote_api/documents
)

list(APPEND LC_REMOTE_API_DEFINES
     LC_REMOTE_API_HAS_DOCUMENTS
)

list(APPEND LC_REMOTE_API_TEST_SOURCES
     librecad/src/remote_api/documents/tests/lc_remoteapi_documents_tests.cpp
)
