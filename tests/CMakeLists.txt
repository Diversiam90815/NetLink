include(Testing)

set(Test_Directory ${CMAKE_CURRENT_SOURCE_DIR})


set(Discovery_Tests
    ${Test_Directory}/DiscoveryTests/EndpointTests.cpp 
    ${Test_Directory}/DiscoveryTests/DiscoveryServiceTests.cpp
    ${Test_Directory}/DiscoveryTests/RegistryTests.cpp
)

)

set(Connection_Tests
    ${Test_Directory}/ConnectionTests/RetryPolicyTests.cpp
    ${Test_Directory}/ConnectionTests/ReadySyncTrackerTests.cpp
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

set(Socket_Tests
    ${Test_Directory}/SocketTests/IPv4AddressTests.cpp
    ${Test_Directory}/SocketTests/UdpSocketTests.cpp
    ${Test_Directory}/SocketTests/FakeDatagramTests.cpp
)

set(Integration_Tests
    ${Test_Directory}/IntegrationTests/NetLinkCoreTests.cpp
)

set(Test_Fakes
    ${Test_Directory}/Fakes/TestIp.h
    ${Test_Directory}/Fakes/FakeDatagramNetwork.h
    ${Test_Directory}/Fakes/LossyDatagramSocket.h
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
    ${Connection_Tests}
    ${Network_Tests}
    ${Utils_Tests}
    ${Validation_Tests}
    ${Socket_Tests}
    ${Integration_Tests}
)


add_executable(NetLinkTests ${All_Tests} ${Test_Fakes})


target_link_libraries(NetLinkTests PRIVATE
    NetLink
    nlohmann_json::nlohmann_json
)

if(MSVC)
    target_compile_options(NetLinkTests PRIVATE /W4)
else()
    target_compile_options(NetLinkTests PRIVATE -Wall -Wextra)
endif()

target_include_directories(NetLinkTests PRIVATE
    ${PROJECT_BINARY_DIR}/generated
    ${PROJECT_SOURCE_DIR}/src/internal
    ${PROJECT_SOURCE_DIR}/src/internal/Discovery
    ${PROJECT_SOURCE_DIR}/src/internal/PeerValidation
    ${PROJECT_SOURCE_DIR}/src/internal/PeerValidation/Checks
    ${PROJECT_SOURCE_DIR}/src/internal/Util
    ${Test_Directory}/Fakes
)

AddTests(NetLinkTests)

source_group(TREE ${Test_Directory} FILES ${All_Tests} ${Test_Fakes})
