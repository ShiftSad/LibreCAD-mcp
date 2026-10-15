# librecad-mcp: Remote API core component (HTTP server, router, GUI runner,
# settings, controller, Preferences tab). Included by ../remote_api.cmake.
# Every file listed here is also listed in core.pri (the tests are CMake-only).

list(APPEND LC_REMOTE_API_SOURCES
     librecad/src/remote_api/core/lc_remoteapiconfig.cpp
     librecad/src/remote_api/core/lc_remoteapiconfig.h
     librecad/src/remote_api/core/lc_remoteapicontext.cpp
     librecad/src/remote_api/core/lc_remoteapicontext.h
     librecad/src/remote_api/core/lc_remoteapicontroller.cpp
     librecad/src/remote_api/core/lc_remoteapicontroller.h
     librecad/src/remote_api/core/lc_remoteapigui.cpp
     librecad/src/remote_api/core/lc_remoteapigui.h
     librecad/src/remote_api/core/lc_remoteapihttp.cpp
     librecad/src/remote_api/core/lc_remoteapihttp.h
     librecad/src/remote_api/core/lc_remoteapijson.cpp
     librecad/src/remote_api/core/lc_remoteapijson.h
     librecad/src/remote_api/core/lc_remoteapimodalguard.cpp
     librecad/src/remote_api/core/lc_remoteapimodalguard.h
     librecad/src/remote_api/core/lc_remoteapioptionswidget.cpp
     librecad/src/remote_api/core/lc_remoteapioptionswidget.h
     librecad/src/remote_api/core/lc_remoteapirouter.cpp
     librecad/src/remote_api/core/lc_remoteapirouter.h
     librecad/src/remote_api/core/lc_remoteapiroutes.h
     librecad/src/remote_api/core/lc_remoteapiserver.cpp
     librecad/src/remote_api/core/lc_remoteapiserver.h
     librecad/src/remote_api/core/lc_remoteapitypes.h
)

list(APPEND LC_REMOTE_API_INCLUDES
     librecad/src/remote_api/core
)

list(APPEND LC_REMOTE_API_TEST_SOURCES
     librecad/src/remote_api/core/tests/lc_remoteapi_core_tests.cpp
     librecad/src/remote_api/core/tests/lc_remoteapi_server_tests.cpp
)
