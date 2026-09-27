include(Benchmarking)

set(Benchmark_Directory ${CMAKE_CURRENT_SOURCE_DIR})



set(Queue_Benchmarks
        ${Benchmark_Directory}/QueueBenchmarks/BoundedQueueBenchmarks.cpp
        ${Benchmark_Directory}/QueueBenchmarks/SequenceBufferBenchmarks.cpp
        ${Benchmark_Directory}/QueueBenchmarks/TaskQueueBenchmarks.cpp
)

set(Timeout_Benchmarks
        ${Benchmark_Directory}/TimeoutBenchmarks/TimeoutServiceBenchmarks.cpp
)

set(Channel_Benchmarks
        ${Benchmark_Directory}/ChannelBenchmarks/PacketCodecBenchmarks.cpp
        ${Benchmark_Directory}/ChannelBenchmarks/SignalPacketBenchmarks.cpp
        ${Benchmark_Directory}/ChannelBenchmarks/FragmentationBenchmarks.cpp
        ${Benchmark_Directory}/ChannelBenchmarks/ReliableLinkBenchmarks.cpp
        ${Benchmark_Directory}/ChannelBenchmarks/HeartbeatRttBenchmarks.cpp
        ${Benchmark_Directory}/ChannelBenchmarks/PeerChannelBenchmarks.cpp
)

set(Validation_Benchmarks
        ${Benchmark_Directory}/ValidationBenchmarks/CompatibilityCheckBenchmarks.cpp
        ${Benchmark_Directory}/ValidationBenchmarks/ValidationStoreBenchmarks.cpp
        ${Benchmark_Directory}/ValidationBenchmarks/PeerValidationBenchmarks.cpp
)

set(Connection_Benchmarks
        ${Benchmark_Directory}/ConnectionBenchmarks/ConnectionServiceBenchmarks.cpp
)

set(Discovery_Benchmarks
        ${Benchmark_Directory}/DiscoveryBenchmarks/DiscoveryBenchmarks.cpp
)
)

set(Benchmark_Support
        ${Benchmark_Directory}/Support/BenchUtil.h
        ${Benchmark_Directory}/Support/LinkWire.h
        ${Benchmark_Directory}/Support/LoopbackChannelPair.h
)
set(All_Benchmarks
        ${Queue_Benchmarks}
        ${Timeout_Benchmarks}
        ${Channel_Benchmarks}
        ${Validation_Benchmarks}
        ${Connection_Benchmarks}
        ${Discovery_Benchmarks}
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
