###########################################
#   NetLink
#       by Jens W. Langenberg
###########################################

cmake_minimum_required(VERSION 4.0)

set(PROJECT_VERSION 0.2.0.85)

project(NetLink VERSION ${PROJECT_VERSION} LANGUAGES CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 23)
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
file(GLOB_RECURSE NETWORKING_INTERNAL_HEADERS src/*.h)

# NetworkInformation has one backend per platform
list(FILTER NETWORKING_SOURCES EXCLUDE REGEX ".*NetworkInformation(Windows|Linux)\\.cpp$")

if(WIN32)
    list(APPEND NETWORKING_SOURCES src/internal/Network/NetworkInformationWindows.cpp)
elseif(APPLE)
    list(APPEND NETWORKING_SOURCES src/internal/Network/NetworkInformationMac.mm)
elseif(UNIX)
    list(APPEND NETWORKING_SOURCES src/internal/Network/NetworkInformationLinux.cpp)
endif()

add_library(${PROJECT_NAME} STATIC ${NETWORKING_SOURCES} ${NETWORKING_HEADERS} ${NETWORKING_INTERNAL_HEADERS})

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src PREFIX "Source Files" FILES ${NETWORKING_SOURCES} ${NETWORKING_INTERNAL_HEADERS})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/include PREFIX "Header Files" FILES ${NETWORKING_HEADERS})

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
    ASIO_NO_DEPRECATED
)

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PUBLIC
        _WIN32_WINNT=0x0A00
        WIN32_LEAN_AND_MEAN
    )
endif()

if(NETLINK_ENABLE_LOGGING)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NETLINK_LOGGING_ENABLED)
    target_link_libraries(${PROJECT_NAME} PRIVATE Logger)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PUBLIC iphlpapi ws2_32 wlanapi)
elseif(APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "-framework CoreWLAN"
        "-framework Foundation"
        "-framework SystemConfiguration"
        "-framework CoreFoundation"
    )
elseif(UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(NL REQUIRED libnl-genl-3.0)
    target_include_directories(${PROJECT_NAME} PRIVATE ${NL_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${NL_LIBRARIES})
endif()

option(NETLINK_BUILD_TESTS "Build NetLink tests" ${PROJECT_IS_TOP_LEVEL})

if(NETLINK_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()
