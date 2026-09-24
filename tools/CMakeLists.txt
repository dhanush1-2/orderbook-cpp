# Tools reuse the benchmark scenario generators, so they need the bench and repo
# roots on the include path. They are not on any measured hot path.
add_executable(ob_replay replay.cpp)
target_link_libraries(ob_replay PRIVATE ob)
target_include_directories(ob_replay PRIVATE
    ${CMAKE_SOURCE_DIR}/bench
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR})
