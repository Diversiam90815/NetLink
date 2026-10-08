include(Testing)

set(Test_Directory ${CMAKE_CURRENT_SOURCE_DIR})


set(Discovery_Tests
    ${Test_Directory}/DiscoveryTests/BeaconTests.cpp
    ${Test_Directory}/DiscoveryTests/DiscoveryLogicTests.cpp
)

set(Session_Tests
    ${Test_Directory}/SessionTests/ControlMessageTests.cpp
)

set(Channel_Tests
    ${Test_Directory}/ChannelTests/PacketFlagsTests.cpp
    ${Test_Directory}/ChannelTests/PacketCodecTests.cpp
    ${Test_Directory}/ChannelTests/AckRangesTests.cpp
    ${Test_Directory}/ChannelTests/SequenceBufferTests.cpp
    ${Test_Directory}/ChannelTests/RttEstimatorTests.cpp
    ${Test_Directory}/ChannelTests/MessageAssemblerTests.cpp
    ${Test_Directory}/ChannelTests/MediaAssemblerTests.cpp
    ${Test_Directory}/ChannelTests/ReliableLinkTests.cpp
    ${Test_Directory}/ChannelTests/LinkMemoryTests.cpp
    ${Test_Directory}/ChannelTests/MailboxTests.cpp
    ${Test_Directory}/ChannelTests/SendSchedulerTests.cpp
)

set(Engine_Tests
    ${Test_Directory}/EngineTests/NetworkEngineTests.cpp
)

set(Network_Tests
    ${Test_Directory}/NetworkTests/AdapterTests.cpp
)

set(Utils_Tests
    ${Test_Directory}/UtilsTests/DeadlineTimerTests.cpp
    ${Test_Directory}/UtilsTests/TaskQueueTests.cpp
)

set(Socket_Tests
    ${Test_Directory}/SocketTests/IPv4AddressTests.cpp
    ${Test_Directory}/SocketTests/UdpSocketTests.cpp
    ${Test_Directory}/SocketTests/FakeDatagramTests.cpp
)

set(Sim_Tests
    ${Test_Directory}/SimTests/PacingSimTests.cpp
    ${Test_Directory}/SimTests/SessionSimTests.cpp
    ${Test_Directory}/SimTests/CoexistenceSimTests.cpp
)

set(Integration_Tests
    ${Test_Directory}/IntegrationTests/NetLinkCoreTests.cpp
)

set(Api_Tests
    ${Test_Directory}/ApiTests/NetLinkApiTests.cpp
)

set(Test_Fakes
    ${Test_Directory}/Fakes/TestIp.h
    ${Test_Directory}/Fakes/FakeDatagramNetwork.h
    ${Test_Directory}/Fakes/LossyDatagramSocket.h
    ${Test_Directory}/Fakes/SpyDatagramSocket.h
    ${Test_Directory}/Fakes/SimDriver.h
    ${Test_Directory}/Fakes/SimScenario.h
    ${Test_Directory}/Fakes/EngineHarness.h
    ${Test_Directory}/Fakes/QueueSource.h
    ${Test_Directory}/Fakes/DeadlineTimer.h
    ${Test_Directory}/Fakes/AllocationCounter.h
    ${Test_Directory}/Fakes/AllocationCounter.cpp
)

# The timer the fake network waits on has one backend per platform
if (WIN32)
    list(APPEND Test_Fakes ${Test_Directory}/Fakes/DeadlineTimerWindows.cpp)
else ()
    list(APPEND Test_Fakes ${Test_Directory}/Fakes/DeadlineTimerStd.cpp)
endif ()


set(All_Tests
    ${Discovery_Tests}
    ${Session_Tests}
    ${Channel_Tests}
    ${Engine_Tests}
    ${Network_Tests}
    ${Utils_Tests}
    ${Socket_Tests}
    ${Integration_Tests}
    ${Api_Tests}
    ${Sim_Tests}
)


add_executable(NetLinkTests ${All_Tests} ${Test_Fakes})


target_link_libraries(NetLinkTests PRIVATE NetLinkInternal)

if(MSVC)
    target_compile_options(NetLinkTests PRIVATE /W4)
else()
    target_compile_options(NetLinkTests PRIVATE -Wall -Wextra)
endif()

target_include_directories(NetLinkTests PRIVATE ${Test_Directory}/Fakes)

AddTests(NetLinkTests)

source_group(TREE ${Test_Directory} FILES ${All_Tests} ${Test_Fakes})
