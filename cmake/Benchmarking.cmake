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

    # Smoke run: every benchmark once, so they keep compiling and running without costing CI time
    if(BUILD_TESTING)
        add_test(NAME ${target}.Smoke COMMAND ${target} --benchmark_min_time=1x)
        set_tests_properties(${target}.Smoke PROPERTIES LABELS benchmark)
    endif()
endmacro()
