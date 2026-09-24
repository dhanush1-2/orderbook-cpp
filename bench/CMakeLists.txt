# Header-only for now; executables arrive in Tasks 11, 12 and 15.
add_library(ob_bench INTERFACE)
target_include_directories(ob_bench INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ob_bench INTERFACE ob)
