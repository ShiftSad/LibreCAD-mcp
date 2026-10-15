# librecad-mcp: Remote API entities component (entities, selection, measure,
# layers, blocks). Included by ../remote_api.cmake. Every source file listed
# here is also listed in entities.pri (the tests are CMake-only and run in
# librecad_remote_api_tests).

list(APPEND LC_REMOTE_API_SOURCES
     librecad/src/remote_api/entities/lc_remoteapientityfactory.cpp
     librecad/src/remote_api/entities/lc_remoteapientityfactory.h
     librecad/src/remote_api/entities/lc_remoteapientityindex.cpp
     librecad/src/remote_api/entities/lc_remoteapientityindex.h
     librecad/src/remote_api/entities/lc_remoteapientityjson.cpp
     librecad/src/remote_api/entities/lc_remoteapientityjson.h
     librecad/src/remote_api/entities/lc_remoteapientityops.cpp
     librecad/src/remote_api/entities/lc_remoteapientityops.h
     librecad/src/remote_api/entities/lc_remoteapientityroutes.h
     librecad/src/remote_api/entities/lc_remoteapilayerops.cpp
     librecad/src/remote_api/entities/lc_remoteapilayerops.h
     librecad/src/remote_api/entities/lc_remoteapiroutes_entities.cpp
)

list(APPEND LC_REMOTE_API_INCLUDES
     librecad/src/remote_api/entities
)

list(APPEND LC_REMOTE_API_DEFINES LC_REMOTE_API_HAS_ENTITIES)

list(APPEND LC_REMOTE_API_TEST_SOURCES
     librecad/src/remote_api/entities/tests/lc_remoteapi_entities_testsupport.h
     librecad/src/remote_api/entities/tests/lc_remoteapi_entities_tests.cpp
     librecad/src/remote_api/entities/tests/lc_remoteapi_layers_tests.cpp
)
