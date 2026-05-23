include(Testing)

add_executable(NetLinkTests
    test_role_negotiation.cpp
    test_signal_packet.cpp
    test_discovery_endpoint.cpp
    test_timeout_service.cpp
    test_validation_types.cpp
    test_peer_validation.cpp
    test_thread_base.cpp
    test_network_adapter.cpp
    test_remote_communication.cpp
)

target_link_libraries(NetLinkTests PRIVATE
    NetLink
    nlohmann_json::nlohmann_json
)

target_include_directories(NetLinkTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/internal
    ${CMAKE_SOURCE_DIR}/src/internal/Discovery
    ${CMAKE_SOURCE_DIR}/src/internal/Util
    ${asio_SOURCE_DIR}/asio/include
)

AddTests(NetLinkTests)
