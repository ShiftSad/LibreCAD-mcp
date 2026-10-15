# librecad-mcp: Remote API entities component (entities, selection, measure,
# layers, blocks). Included by ../remote_api.pri. Every source file listed
# here is also listed in entities.cmake (the tests are built by CMake only).

INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

DEFINES += LC_REMOTE_API_HAS_ENTITIES

HEADERS += \
    $$PWD/lc_remoteapientityfactory.h \
    $$PWD/lc_remoteapientityindex.h \
    $$PWD/lc_remoteapientityjson.h \
    $$PWD/lc_remoteapientityops.h \
    $$PWD/lc_remoteapientityroutes.h \
    $$PWD/lc_remoteapilayerops.h

SOURCES += \
    $$PWD/lc_remoteapientityfactory.cpp \
    $$PWD/lc_remoteapientityindex.cpp \
    $$PWD/lc_remoteapientityjson.cpp \
    $$PWD/lc_remoteapientityops.cpp \
    $$PWD/lc_remoteapilayerops.cpp \
    $$PWD/lc_remoteapiroutes_entities.cpp
