###########################################
#   NetLink
#       by Jens W. Langenberg
###########################################

cmake_minimum_required(VERSION 4.0)

set(PROJECT_VERSION 0.1.0.9)

project(NetLink VERSION ${PROJECT_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(cpm)

CPMAddPackage(
  NAME asio
  GITHUB_REPOSITORY chriskohlhoff/asio
  GIT_TAG asio-1-30-2
  OPTIONS "ASIO_STANDALONE ON"
)

CPMAddPackage(
  NAME Logger
  GITHUB_REPOSITORY Diversiam90815/Logger
  VERSION 1.3.81
)

file(GLOB_RECURSE NETWORKING_SOURCES src/*.cpp)
file(GLOB_RECURSE NETWORKING_HEADERS include/*.h)

add_library(${PROJECT_NAME} STATIC ${NETWORKING_SOURCES} ${NETWORKING_HEADERS})

target_include_directories(${PROJECT_NAME} PUBLIC include)
target_include_directories(${PROJECT_NAME} PRIVATE ${asio_SOURCE_DIR}/asio/include)

target_compile_definitions(${PROJECT_NAME} PUBLIC
    ASIO_STANDALONE
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(${PROJECT_NAME} PUBLIC Logger iphlpapi)
