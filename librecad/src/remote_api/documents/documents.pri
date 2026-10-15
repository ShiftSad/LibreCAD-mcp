# librecad-mcp: Remote API documents component (formats, documents, I/O,
# drawing info, render, view, undo/redo, command line). Included by
# ../remote_api.pri. Every file listed here is also listed in documents.cmake.
# The tests (tests/*.cpp) are built by CMake only.

INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

DEFINES += LC_REMOTE_API_HAS_DOCUMENTS

HEADERS += \
    $$PWD/lc_remoteapiappaccess.h \
    $$PWD/lc_remoteapidocumentsstorage.h \
    $$PWD/lc_remoteapiformats.h \
    $$PWD/lc_remoteapipathpolicy.h \
    $$PWD/lc_remoteapirenderer.h

SOURCES += \
    $$PWD/lc_remoteapiappaccess.cpp \
    $$PWD/lc_remoteapidocumentsstorage.cpp \
    $$PWD/lc_remoteapiformats.cpp \
    $$PWD/lc_remoteapipathpolicy.cpp \
    $$PWD/lc_remoteapirenderer.cpp \
    $$PWD/lc_remoteapiroutes_documents.cpp
