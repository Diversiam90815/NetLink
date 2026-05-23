###########################################
#   NetLink
#       by Jens W. Langenberg
###########################################

cmake_minimum_required(VERSION 4.0)

set(PROJECT_VERSION 0.1.0.85)

project(NetLink VERSION ${PROJECT_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

option(NETLINK_ENABLE_LOGGING "Enable logging via Logger library" OFF)

include(cpm)

CPMAddPackage(
  NAME asio
  GITHUB_REPOSITORY chriskohlhoff/asio
  GIT_TAG asio-1-30-2
  OPTIONS "ASIO_STANDALONE ON"
)

if(NETLINK_ENABLE_LOGGING)
    CPMAddPackage(
      NAME Logger
      GITHUB_REPOSITORY Diversiam90815/Logger
      VERSION 1.3.81
    )
endif()

CPMAddPackage(
  NAME nlohmann_json
  GITHUB_REPOSITORY nlohmann/json
  VERSION 3.11.3
)

file(GLOB_RECURSE NETWORKING_SOURCES src/*.cpp)
file(GLOB_RECURSE NETWORKING_HEADERS include/*.h)

add_library(${PROJECT_NAME} STATIC ${NETWORKING_SOURCES} ${NETWORKING_HEADERS})

target_include_directories(${PROJECT_NAME} PUBLIC include)

target_include_directories(${PROJECT_NAME} 
    PRIVATE 
        ${asio_SOURCE_DIR}/asio/include
        src/internal/
        src/internal/Discovery
        src/internal/Util
)

target_compile_definitions(${PROJECT_NAME} PUBLIC
    ASIO_STANDALONE
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
)

if(NETLINK_ENABLE_LOGGING)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NETLINK_LOGGING_ENABLED)
    target_link_libraries(${PROJECT_NAME} PRIVATE Logger)
endif()

target_link_libraries(${PROJECT_NAME} PUBLIC iphlpapi PRIVATE nlohmann_json::nlohmann_json)

option(NETLINK_BUILD_TESTS "Build NetLink tests" ${PROJECT_IS_TOP_LEVEL})

if(NETLINK_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()
