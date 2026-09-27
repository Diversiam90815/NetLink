include(Benchmarking)

set(Benchmark_Directory ${CMAKE_CURRENT_SOURCE_DIR})


set(Timeout_Benchmarks
    ${Benchmark_Directory}/TimeoutBenchmarks/TimeoutServiceBenchmarks.cpp
)

set(Benchmark_Support
        ${Benchmark_Directory}/Support/BenchUtil.h
        ${Benchmark_Directory}/Support/LinkWire.h
        ${Benchmark_Directory}/Support/LoopbackChannelPair.h
)
set(All_Benchmarks
    ${Timeout_Benchmarks}
)
add_executable(NetLinkBenchmarks ${All_Benchmarks} ${Benchmark_Support})

target_link_libraries(NetLinkBenchmarks PRIVATE NetLinkInternal)

if(MSVC)
    target_compile_options(NetLinkBenchmarks PRIVATE /W4)
else()
    target_compile_options(NetLinkBenchmarks PRIVATE -Wall -Wextra)
endif()

target_include_directories(NetLinkBenchmarks PRIVATE ${Benchmark_Directory}/Support)

AddBenchmarks(NetLinkBenchmarks)

source_group(TREE ${Benchmark_Directory} FILES ${All_Benchmarks} ${Benchmark_Support})
