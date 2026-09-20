###########################################
#   NetLink
#       by Jens W. Langenberg
###########################################

cmake_minimum_required(VERSION 4.0)

set(PROJECT_VERSION 0.2.0)
set(NETLINK_BUILD_NUMBER "0" CACHE STRING "Build number")

project(NetLink VERSION ${PROJECT_VERSION} LANGUAGES CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(NETLINK_ENABLE_LOGGING "Enable logging via Logger library" OFF)

include(cpm)

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

file(GLOB_RECURSE NETWORKING_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB_RECURSE NETWORKING_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)
file(GLOB_RECURSE NETWORKING_INTERNAL_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h)

# NetworkInformation and the socket platform shim have one backend per platform
list(FILTER NETWORKING_SOURCES EXCLUDE REGEX ".*NetworkInformation(Windows|Linux)\\.cpp$")
list(FILTER NETWORKING_SOURCES EXCLUDE REGEX ".*SocketPlatform(Windows|Posix)\\.cpp$")

if(WIN32)
    list(APPEND NETWORKING_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal/Network/NetworkInformationWindows.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal/Socket/Platform/SocketPlatformWindows.cpp
    )
elseif(APPLE)
    list(APPEND NETWORKING_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal/Network/NetworkInformationMac.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal/Socket/Platform/SocketPlatformPosix.cpp
    )
elseif(UNIX)
    list(APPEND NETWORKING_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal/Network/NetworkInformationLinux.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal/Socket/Platform/SocketPlatformPosix.cpp
    )
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/NetLinkVersion.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/NetLinkVersion.h
    @ONLY)

add_library(${PROJECT_NAME} STATIC ${NETWORKING_SOURCES} ${NETWORKING_HEADERS} ${NETWORKING_INTERNAL_HEADERS})
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src PREFIX "Source Files" FILES ${NETWORKING_SOURCES} ${NETWORKING_INTERNAL_HEADERS})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/include PREFIX "Header Files" FILES ${NETWORKING_HEADERS})

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_include_directories(${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal/Discovery
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal/Util
)

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _WIN32_WINNT=0x0A00
        WIN32_LEAN_AND_MEAN
    )
endif()

if(NETLINK_ENABLE_LOGGING)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NETLINK_LOGGING_ENABLED)
    target_link_libraries(${PROJECT_NAME} PRIVATE Logger)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE iphlpapi ws2_32 wlanapi)
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

# ---------------------------------------------------------------------------
# Install / export, so consumers can use find_package(NetLink) as well as CPM
# ---------------------------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS ${PROJECT_NAME}
    EXPORT NetLinkTargets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT NetLinkTargets
    FILE      NetLinkTargets.cmake
    NAMESPACE NetLink::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NetLink)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/NetLinkConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/NetLinkConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/NetLinkConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NetLink)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/NetLinkConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/NetLinkConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NetLink)


option(NETLINK_BUILD_TESTS "Build NetLink tests" ${PROJECT_IS_TOP_LEVEL})

if(NETLINK_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()
