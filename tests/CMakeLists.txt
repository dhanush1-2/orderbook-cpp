include(FetchContent)
# SYSTEM is load-bearing: without it GoogleTest's own headers trip our
# -Wold-style-cast and the build fails on code we do not own.
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
    SYSTEM)
FetchContent_MakeAvailable(googletest)

add_executable(ob_tests
    test_smoke.cpp
    test_clock.cpp
    test_histogram.cpp
    test_order_pool.cpp
    test_id_index.cpp
    test_level_bitmap.cpp
    test_price_ladder.cpp
    test_fast_engine.cpp
    test_differential.cpp
    test_scenarios.cpp
    test_seqlock.cpp
    test_l2_snapshot.cpp
    test_tui.cpp
    test_publisher_isolation.cpp
    test_wire.cpp
    test_histogram.cpp
    test_order_pool.cpp
    test_id_index.cpp
    test_level_bitmap.cpp
    test_price_ladder.cpp
    test_fast_engine.cpp
    test_differential.cpp
    test_scenarios.cpp
    test_seqlock.cpp
    test_l2_snapshot.cpp
    test_tui.cpp
    test_publisher_isolation.cpp
    test_wire.cpp
    test_order_pool.cpp
    test_id_index.cpp
    test_level_bitmap.cpp
    test_price_ladder.cpp
    test_fast_engine.cpp
    test_differential.cpp
    test_scenarios.cpp
    test_seqlock.cpp
    test_l2_snapshot.cpp
    test_tui.cpp
    test_publisher_isolation.cpp
    test_wire.cpp
    test_types.cpp
    test_command_events.cpp
    test_event_buffer.cpp
    test_reference_validation.cpp
    test_reference_resting.cpp
    test_reference_matching.cpp
    test_reference_cancel.cpp
    test_reference_order_types.cpp
    test_edge_cases.cpp
    test_invariants.cpp
    test_scenario_gen.cpp
    test_determinism.cpp
    test_stress.cpp
)
target_link_libraries(ob_tests PRIVATE ob GTest::gtest_main)

# Death tests rely on assert(), which NDEBUG would delete. Release adds -DNDEBUG
# via CMAKE_CXX_FLAGS_RELEASE; target options land after it on the command line,
# so this undefines it for the test binary only.
target_compile_options(ob_tests PRIVATE -UNDEBUG)
target_include_directories(ob_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

include(GoogleTest)
gtest_discover_tests(ob_tests)

# Its own binary on purpose: replacing global operator new inside ob_tests would
# perturb GoogleTest's own allocations and make the assertion meaningless.
if(OB_BUILD_BENCH)
    add_executable(ob_test_alloc_counter test_alloc_counter.cpp)
    target_link_libraries(ob_test_alloc_counter PRIVATE ob ob_alloc_counter)
    target_compile_options(ob_test_alloc_counter PRIVATE -UNDEBUG)
    add_test(NAME alloc_counter COMMAND ob_test_alloc_counter)
endif()
