# librecad-mcp: Remote API core component (HTTP server, router, GUI runner,
# settings, controller, Preferences tab). Included by ../remote_api.pri.
# Every file listed here is also listed in core.cmake (the tests are CMake-only).

INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

# the server reads and writes its connections through the native socket API
win32: LIBS += -lws2_32

HEADERS += \
    $$PWD/lc_remoteapiconfig.h \
    $$PWD/lc_remoteapicontext.h \
    $$PWD/lc_remoteapicontroller.h \
    $$PWD/lc_remoteapigui.h \
    $$PWD/lc_remoteapihttp.h \
    $$PWD/lc_remoteapijson.h \
    $$PWD/lc_remoteapimodalguard.h \
    $$PWD/lc_remoteapioptionswidget.h \
    $$PWD/lc_remoteapirouter.h \
    $$PWD/lc_remoteapiroutes.h \
    $$PWD/lc_remoteapiserver.h \
    $$PWD/lc_remoteapitypes.h

SOURCES += \
    $$PWD/lc_remoteapiconfig.cpp \
    $$PWD/lc_remoteapicontext.cpp \
    $$PWD/lc_remoteapicontroller.cpp \
    $$PWD/lc_remoteapigui.cpp \
    $$PWD/lc_remoteapihttp.cpp \
    $$PWD/lc_remoteapijson.cpp \
    $$PWD/lc_remoteapimodalguard.cpp \
    $$PWD/lc_remoteapioptionswidget.cpp \
    $$PWD/lc_remoteapirouter.cpp \
    $$PWD/lc_remoteapiserver.cpp
