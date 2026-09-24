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
    test_types.cpp
    test_command_events.cpp
    test_event_buffer.cpp
    test_reference_validation.cpp
    test_reference_resting.cpp
    test_reference_matching.cpp
    test_reference_cancel.cpp
)
target_link_libraries(ob_tests PRIVATE ob GTest::gtest_main)

# Death tests rely on assert(), which NDEBUG would delete. Release adds -DNDEBUG
# via CMAKE_CXX_FLAGS_RELEASE; target options land after it on the command line,
# so this undefines it for the test binary only.
target_compile_options(ob_tests PRIVATE -UNDEBUG)

include(GoogleTest)
gtest_discover_tests(ob_tests)
