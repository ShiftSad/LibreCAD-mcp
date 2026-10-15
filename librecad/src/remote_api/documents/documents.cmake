# librecad-mcp: Remote API documents component (formats, documents, I/O,
# drawing info, render, view, undo/redo, command line). Included by
# ../remote_api.cmake. Every file listed here is also listed in documents.pri.

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

# Tests. Once the core component has its implementation (lc_remoteapirouter.cpp
# and the other core sources), the documents tests join the shared
# librecad_remote_api_tests target of the top-level CMakeLists.txt. Until then
# they build as librecad_remote_api_documents_tests, which links test-only
# stand-ins for the core functions (tests/lc_remoteapi_core_stubs.cpp); the two
# must never be linked together.
set(LC_REMOTE_API_DOCUMENTS_DIR "${CMAKE_CURRENT_LIST_DIR}")
if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/../core/lc_remoteapirouter.cpp")
    list(APPEND LC_REMOTE_API_TEST_SOURCES
         librecad/src/remote_api/documents/tests/lc_remoteapi_documents_tests.cpp
    )
else ()
    # The test sources, Catch2 and the shared test helpers are defined further
    # down in the top-level CMakeLists.txt: define the target at its end.
    function(lc_remote_api_add_documents_tests)
        if (NOT BUILD_TESTS)
            return()
        endif ()
        qt_add_executable(librecad_remote_api_documents_tests
            EXCLUDE_FROM_ALL
            librecad/src/main/lc_release_label.cpp
            ${LIBRECAD_TEST_SETTINGS_ISOLATION_SOURCE}
            ${LIBRECAD_TEST_TEMP_DIRECTORY_SOURCE}
            ${LIBRECAD_RES}
            "${LC_REMOTE_API_DOCUMENTS_DIR}/tests/lc_remoteapi_documents_tests.cpp"
            "${LC_REMOTE_API_DOCUMENTS_DIR}/tests/lc_remoteapi_core_stubs.cpp"
        )
        set_target_properties(librecad_remote_api_documents_tests PROPERTIES
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC ON
        )
        target_include_directories(librecad_remote_api_documents_tests PRIVATE ${SHARED_INCLUDES})
        target_link_libraries(librecad_remote_api_documents_tests PRIVATE
                               librecad_lib
                               Catch2::Catch2WithMain
                               Qt6::Core
                               Qt6::Gui
                               Qt6::Network
                               Qt6::PrintSupport
                               Qt6::Svg
                               Qt6::Widgets
        )
        target_compile_features(librecad_remote_api_documents_tests PRIVATE cxx_std_17)
        if (MSVC)
            target_compile_options(librecad_remote_api_documents_tests PRIVATE /bigobj)
        endif ()
        target_compile_definitions(librecad_remote_api_documents_tests PRIVATE
            BUILD_TESTS=1
            DWGSUPPORT
            _USE_MATH_DEFINES
            LIBRECAD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
            LIBRECAD_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/librecad/src/lib/filters/tests/testdata"
            ${LC_REMOTE_API_DEFINES}
        )
        add_test(NAME librecad_remote_api_documents_tests
            COMMAND $<TARGET_FILE:librecad_remote_api_documents_tests> --reporter compact)
        set_tests_properties(librecad_remote_api_documents_tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            TIMEOUT 300)
    endfunction()
    cmake_language(DEFER CALL lc_remote_api_add_documents_tests)
endif ()
