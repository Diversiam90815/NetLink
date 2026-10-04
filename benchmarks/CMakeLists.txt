include(Benchmarking)

set(Benchmark_Directory ${CMAKE_CURRENT_SOURCE_DIR})


set(Socket_Benchmarks
        ${Benchmark_Directory}/SocketBenchmarks/UdpSocketBenchmarks.cpp
)

set(Channel_Benchmarks
        ${Benchmark_Directory}/ChannelBenchmarks/ReliableLinkBenchmarks.cpp
        ${Benchmark_Directory}/ChannelBenchmarks/FragmentationBenchmarks.cpp
)

set(Engine_Benchmarks
        ${Benchmark_Directory}/EngineBenchmarks/EngineBenchmarks.cpp
)

set(Utils_Benchmarks
        ${Benchmark_Directory}/UtilsBenchmarks/TaskQueueBenchmarks.cpp
)

set(Integration_Benchmarks
        ${Benchmark_Directory}/IntegrationBenchmarks/NetLinkCoreBenchmarks.cpp
)

set(Benchmark_Support
        ${Benchmark_Directory}/Support/BenchUtil.h
        ${Benchmark_Directory}/Support/Loopback.h
        ${Benchmark_Directory}/Support/LinkWire.h
)


set(All_Benchmarks
        ${Socket_Benchmarks}
        ${Channel_Benchmarks}
        ${Engine_Benchmarks}
        ${Utils_Benchmarks}
        ${Integration_Benchmarks}
)


add_executable(NetLinkBenchmarks ${All_Benchmarks} ${Benchmark_Support})

target_link_libraries(NetLinkBenchmarks PRIVATE NetLinkInternal)

if (MSVC)
    target_compile_options(NetLinkBenchmarks PRIVATE /W4)
else ()
    target_compile_options(NetLinkBenchmarks PRIVATE -Wall -Wextra)
endif ()

target_include_directories(NetLinkBenchmarks PRIVATE ${Benchmark_Directory}/Support)

AddBenchmarks(NetLinkBenchmarks)

source_group(TREE ${Benchmark_Directory} FILES ${All_Benchmarks} ${Benchmark_Support})
