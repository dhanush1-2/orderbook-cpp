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
)
target_link_libraries(ob_tests PRIVATE ob GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(ob_tests)
