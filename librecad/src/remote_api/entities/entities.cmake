# librecad-mcp: Remote API entities component (entities, selection, measure,
# layers, blocks). Included by ../remote_api.cmake. Every source file listed
# here is also listed in entities.pri.

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

# Test-only stand-ins for core implementation files that are not on the
# branch yet (the entities component was written in parallel with the core
# one, against the pinned core headers). Each one drops out of the build by
# itself as soon as the real core/<file>.cpp exists; none is ever part of the
# application.
foreach(_lc_remote_api_core_unit json router context)
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/../core/lc_remoteapi${_lc_remote_api_core_unit}.cpp")
        list(APPEND LC_REMOTE_API_TEST_SOURCES
             librecad/src/remote_api/entities/tests/stubs/lc_remoteapi_stub_${_lc_remote_api_core_unit}.cpp)
    endif()
endforeach()
unset(_lc_remote_api_core_unit)
