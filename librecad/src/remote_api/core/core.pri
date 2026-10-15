# librecad-mcp: Remote API core component (HTTP server, router, GUI runner,
# settings, controller, Preferences tab). Included by ../remote_api.pri.
# Every file listed here is also listed in core.cmake.

INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

HEADERS += \
    $$PWD/lc_remoteapiconfig.h \
    $$PWD/lc_remoteapicontext.h \
    $$PWD/lc_remoteapicontroller.h \
    $$PWD/lc_remoteapigui.h \
    $$PWD/lc_remoteapijson.h \
    $$PWD/lc_remoteapirouter.h \
    $$PWD/lc_remoteapiroutes.h \
    $$PWD/lc_remoteapitypes.h

SOURCES += \
    $$PWD/lc_remoteapicontroller.cpp
