add_executable(benchmarks
    bench_main.cpp
    bench_trie.cpp
    bench_segmented_manager.cpp
)

target_include_directories(benchmarks PRIVATE ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(benchmarks
    PRIVATE
    benchmark::benchmark
    segmented_interprocess
    Threads::Threads
)
