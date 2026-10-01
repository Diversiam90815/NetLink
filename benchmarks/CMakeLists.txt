include(Benchmarking)

set(Benchmark_Directory ${CMAKE_CURRENT_SOURCE_DIR})


set(Socket_Benchmarks
        ${Benchmark_Directory}/SocketBenchmarks/UdpSocketBenchmarks.cpp
)

set(Channel_Benchmarks
        ${Benchmark_Directory}/ChannelBenchmarks/ReliableLinkBenchmarks.cpp
        ${Benchmark_Directory}/ChannelBenchmarks/FragmentationBenchmarks.cpp
        ${Benchmark_Directory}/ChannelBenchmarks/PeerChannelBenchmarks.cpp
)

set(Utils_Benchmarks
        ${Benchmark_Directory}/UtilsBenchmarks/TimeoutServiceBenchmarks.cpp
        ${Benchmark_Directory}/UtilsBenchmarks/TaskQueueBenchmarks.cpp
)

set(Validation_Benchmarks
        ${Benchmark_Directory}/ValidationBenchmarks/PeerValidationBenchmarks.cpp
)

set(Connection_Benchmarks
        ${Benchmark_Directory}/ConnectionBenchmarks/ConnectionServiceBenchmarks.cpp
)

set(Discovery_Benchmarks
        ${Benchmark_Directory}/DiscoveryBenchmarks/DiscoveryBenchmarks.cpp
)

set(Integration_Benchmarks
        ${Benchmark_Directory}/IntegrationBenchmarks/NetLinkCoreBenchmarks.cpp
)

set(Benchmark_Support
        ${Benchmark_Directory}/Support/BenchUtil.h
        ${Benchmark_Directory}/Support/Loopback.h
        ${Benchmark_Directory}/Support/ServiceWiring.h
        ${Benchmark_Directory}/Support/LinkWire.h
)


set(All_Benchmarks
        ${Socket_Benchmarks}
        ${Channel_Benchmarks}
        ${Utils_Benchmarks}
        ${Validation_Benchmarks}
        ${Connection_Benchmarks}
        ${Discovery_Benchmarks}
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
