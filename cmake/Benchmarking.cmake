include(cpm)

CPMAddPackage(
        NAME benchmark
        GITHUB_REPOSITORY google/benchmark
        VERSION 1.9.4
        OPTIONS
        "BENCHMARK_ENABLE_TESTING OFF"
        "BENCHMARK_ENABLE_GTEST_TESTS OFF"
        "BENCHMARK_ENABLE_INSTALL OFF"
        "BENCHMARK_INSTALL_DOCS OFF"
        "BENCHMARK_ENABLE_WERROR OFF"
        )

macro(AddBenchmarks target)
    message("Adding benchmarks to ${target}")
    target_link_libraries(${target} PRIVATE benchmark::benchmark_main)
endmacro()
