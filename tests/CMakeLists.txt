add_executable(async_framework_tests
    test_executor.cpp
    test_executor_extra.cpp
    test_strand.cpp
    test_strand_extra.cpp
    test_completion.cpp
    test_completion_extra.cpp
    test_model.cpp
    test_logger.cpp
    test_backend_extra.cpp
    test_registry_extra.cpp
    test_bridge_local.cpp
    test_bridge_remote.cpp
    test_remote_extra.cpp
    test_dispatch_di.cpp
    test_handler_binding.cpp
    test_switch_backend.cpp
    test_network_monitor.cpp
    test_offline_queue.cpp
    test_sync_worker.cpp
    test_offline_integration.cpp
    test_conflict_resolution.cpp
    test_completion_branches.cpp
    test_task.cpp
    test_issues.cpp
    test_concurrency_invariants.cpp
)

target_link_libraries(async_framework_tests
    PRIVATE
        async_framework::async_framework
        Catch2::Catch2WithMain
)

apply_warnings(async_framework_tests)

if(DEFINED AF_SANITIZER)
    apply_sanitizers(async_framework_tests ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(async_framework_tests)
endif()

if(AF_SANITIZER STREQUAL "msan")
    # catch_discover_tests runs the binary for enumeration which triggers MSan
    # before any suppressions can be applied. Use a single all-up test instead,
    # with the suppression file to silence Catch2's known false positive.
    add_test(NAME async_framework_tests COMMAND async_framework_tests)
else()
    include(Catch)
    catch_discover_tests(async_framework_tests DISCOVERY_MODE PRE_TEST)
endif()
