# Header-only harness pieces.
add_library(ob_bench INTERFACE)
target_include_directories(ob_bench INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ob_bench INTERFACE ob)

# Replaces global operator new to count allocations. Linked ONLY into benchmark
# executables and the dedicated allocation test, never into ob_tests.
add_library(ob_alloc_counter STATIC alloc_counter.cpp)
target_include_directories(ob_alloc_counter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ob_alloc_counter PRIVATE ob_warnings)

add_executable(ob_bench_latency bench_latency.cpp)
target_link_libraries(ob_bench_latency PRIVATE ob ob_bench ob_alloc_counter)
