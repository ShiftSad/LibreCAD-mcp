# librecad-mcp: Remote API core component (HTTP server, router, GUI runner,
# settings, controller, Preferences tab). Included by ../remote_api.cmake.
# Every file listed here is also listed in core.pri.

list(APPEND LC_REMOTE_API_SOURCES
     librecad/src/remote_api/core/lc_remoteapiconfig.h
     librecad/src/remote_api/core/lc_remoteapicontext.h
     librecad/src/remote_api/core/lc_remoteapicontroller.cpp
     librecad/src/remote_api/core/lc_remoteapicontroller.h
     librecad/src/remote_api/core/lc_remoteapigui.h
     librecad/src/remote_api/core/lc_remoteapijson.h
     librecad/src/remote_api/core/lc_remoteapirouter.h
     librecad/src/remote_api/core/lc_remoteapiroutes.h
     librecad/src/remote_api/core/lc_remoteapitypes.h
)

list(APPEND LC_REMOTE_API_INCLUDES
     librecad/src/remote_api/core
)
