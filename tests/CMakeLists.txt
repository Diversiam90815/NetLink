include(Testing)

set(Test_Directory ${CMAKE_CURRENT_SOURCE_DIR})


set(Discovery_Tests
    ${Test_Directory}/DiscoveryTests/EndpointTests.cpp 
    ${Test_Directory}/DiscoveryTests/DiscoveryServiceTests.cpp
)

set(Communication_Tests
    ${Test_Directory}/CommunicationTests/RemoteCommunicationTests.cpp
    ${Test_Directory}/CommunicationTests/SignalPacketTests.cpp
)

set(Connection_Tests
    ${Test_Directory}/ConnectionTests/RetryPolicyTests.cpp
    ${Test_Directory}/ConnectionTests/ReadySyncTrackerTests.cpp
    ${Test_Directory}/ConnectionTests/RoleNegotiationTests.cpp
    ${Test_Directory}/ConnectionTests/ConnectionServiceTests.cpp
)

set(Network_Tests
    ${Test_Directory}/NetworkTests/AdapterTests.cpp
)

set(Utils_Tests
    ${Test_Directory}/UtilsTests/TaskQueueTests.cpp
    ${Test_Directory}/UtilsTests/ThreadBaseTests.cpp
    ${Test_Directory}/UtilsTests/TimeoutServiceTests.cpp
)

set(TCP_Tests
    ${Test_Directory}/TCPTests/TCPTransportTests.cpp
)

set(Validation_Tests
    ${Test_Directory}/ValidationTests/CompatibilityCheckTests.cpp
    ${Test_Directory}/ValidationTests/HandshakeTests.cpp
    ${Test_Directory}/ValidationTests/PeerValidationTests.cpp
    ${Test_Directory}/ValidationTests/PendingValidationStoreTests.cpp
    ${Test_Directory}/ValidationTests/ValidatedPeerRegistryTests.cpp
    ${Test_Directory}/ValidationTests/ValidationTypesTests.cpp
)


set(All_Tests
    ${Discovery_Tests}
    ${Communication_Tests}
    ${Connection_Tests}
    ${Network_Tests}
    ${Utils_Tests}
    ${Validation_Tests}
    ${TCP_Tests}
)


add_executable(NetLinkTests ${All_Tests})


target_link_libraries(NetLinkTests PRIVATE
    NetLink
    nlohmann_json::nlohmann_json
)

target_include_directories(NetLinkTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/internal
    ${CMAKE_SOURCE_DIR}/src/internal/Discovery
    ${CMAKE_SOURCE_DIR}/src/internal/PeerValidation
    ${CMAKE_SOURCE_DIR}/src/internal/PeerValidation/Checks
    ${CMAKE_SOURCE_DIR}/src/internal/Util
    ${asio_SOURCE_DIR}/asio/include
)

AddTests(NetLinkTests)

source_group(TREE ${Test_Directory} FILES ${All_Tests})
