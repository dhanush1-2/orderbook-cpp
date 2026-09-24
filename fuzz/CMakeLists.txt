# libFuzzer is a clang feature. Fail loudly rather than silently building nothing.
#
# TWO PLATFORM FACTS, both found by trying:
#
# 1. Apple clang (Command Line Tools) ships NO libFuzzer runtime: linking fails
#    with "library 'libclang_rt.fuzzer_osx.a' not found". Build this target in
#    Docker with real LLVM clang instead. See scripts/fuzz.sh.
#
# 2. On Alpine/musl, LeakSanitizer reports a spurious 56 bytes leaked in 2
#    allocations AT EXIT, attributed to the empty input. A fuzz target whose body
#    is literally `return 0;` reports the identical leak, so it is libFuzzer's own
#    runtime, not this code. Run with ASAN_OPTIONS=detect_leaks=0. libFuzzer's own
#    -detect_leaks=0 flag does NOT suppress it, because that controls libFuzzer's
#    malloc-balance check rather than LSan's at-exit scan.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "OB_BUILD_FUZZ=ON requires clang (libFuzzer)")
endif()

add_executable(ob_fuzz_differential fuzz_differential.cpp)
target_link_libraries(ob_fuzz_differential PRIVATE ob)
# -UNDEBUG is load-bearing: the invariant asserts inside OrderPool and PriceLadder
# are most of what the fuzzer is here to trip, and NDEBUG would delete them.
target_compile_options(ob_fuzz_differential PRIVATE
    -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -UNDEBUG)
target_link_options(ob_fuzz_differential PRIVATE -fsanitize=fuzzer,address,undefined)
