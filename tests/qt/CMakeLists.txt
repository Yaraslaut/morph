add_executable(async_framework_qt_tests
    test_qt_websocket.cpp
)

target_link_libraries(async_framework_qt_tests
    PRIVATE
        async_framework_qt_impl
        Catch2::Catch2WithMain
)

set_target_properties(async_framework_qt_tests PROPERTIES AUTOMOC ON)
apply_warnings(async_framework_qt_tests)

target_compile_definitions(async_framework_qt_tests PRIVATE
    TESTS_CERTS_DIR="${CMAKE_SOURCE_DIR}/tests/certs"
)

include(Catch)

# Qt DLLs must be findable at both test discovery and run time on Windows.
# DL_PATHS prepends to PATH (Windows) / LD_LIBRARY_PATH (Linux) / DYLD_LIBRARY_PATH (macOS).
get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION)
cmake_path(GET _qt_core_dll PARENT_PATH _qt_bin_dir)

catch_discover_tests(async_framework_qt_tests
    DISCOVERY_MODE POST_BUILD
    DL_PATHS "${_qt_bin_dir}"
)
