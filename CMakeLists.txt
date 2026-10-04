###########################################
#   NetLink
#       by Jens W. Langenberg
###########################################

cmake_minimum_required(VERSION 4.0)

set(PROJECT_VERSION 0.5.0)
set(NETLINK_BUILD_NUMBER "0" CACHE STRING "Build number")

project(NetLink VERSION ${PROJECT_VERSION} LANGUAGES CXX)

if (APPLE)
    enable_language(OBJCXX)
endif ()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(NETLINK_SANITIZER "" CACHE STRING "Build the library, tests and benchmarks with a sanitizer: thread or address (clang, gcc)")

include(cpm)

file(GLOB_RECURSE NETWORKING_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB_RECURSE NETWORKING_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)
file(GLOB_RECURSE NETWORKING_INTERNAL_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h)

# NetworkInformation and the socket platform shim have one backend per platform
list(FILTER NETWORKING_SOURCES EXCLUDE REGEX ".*NetworkInformation(Windows|Linux)\\.cpp$")
list(FILTER NETWORKING_SOURCES EXCLUDE REGEX ".*SocketPlatform(Windows|Posix)\\.cpp$")

if (WIN32)
    list(APPEND NETWORKING_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/internal/Network/NetworkInformationWindows.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/internal/Socket/Platform/SocketPlatformWindows.cpp
    )
elseif (APPLE)
    list(APPEND NETWORKING_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/internal/Network/NetworkInformationMac.mm
            ${CMAKE_CURRENT_SOURCE_DIR}/src/internal/Socket/Platform/SocketPlatformPosix.cpp
    )
elseif (UNIX)
    list(APPEND NETWORKING_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/internal/Network/NetworkInformationLinux.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/internal/Socket/Platform/SocketPlatformPosix.cpp
    )
endif ()

configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/NetLinkVersion.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/generated/NetLinkVersion.h
        @ONLY)

add_library(${PROJECT_NAME} STATIC ${NETWORKING_SOURCES} ${NETWORKING_HEADERS} ${NETWORKING_INTERNAL_HEADERS})
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src PREFIX "Source Files" FILES ${NETWORKING_SOURCES} ${NETWORKING_INTERNAL_HEADERS})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/include PREFIX "Header Files" FILES ${NETWORKING_HEADERS})

if (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else ()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif ()

if (NETLINK_SANITIZER STREQUAL "thread")
    set(NETLINK_SANITIZER_FLAGS -fsanitize=thread)
elseif (NETLINK_SANITIZER STREQUAL "address")
    set(NETLINK_SANITIZER_FLAGS -fsanitize=address -fsanitize=undefined)
elseif (NETLINK_SANITIZER)
    message(FATAL_ERROR "NETLINK_SANITIZER must be 'thread' or 'address'")
endif ()

# PUBLIC: whatever links the library in this build (tests, benchmarks) is instrumented with it
foreach (flag IN LISTS NETLINK_SANITIZER_FLAGS)
    target_compile_options(${PROJECT_NAME} PUBLIC $<BUILD_INTERFACE:${flag}> $<BUILD_INTERFACE:-fno-omit-frame-pointer>)
    target_link_options(${PROJECT_NAME} PUBLIC $<BUILD_INTERFACE:${flag}>)
endforeach ()

target_include_directories(${PROJECT_NAME} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_include_directories(${PROJECT_NAME}
        PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal/Util
)

if (WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
            _WIN32_WINNT=0x0A00
            WIN32_LEAN_AND_MEAN
    )
endif ()

if (WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE iphlpapi ws2_32 winmm)
elseif (APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
            "-framework Foundation"
            "-framework SystemConfiguration"
            "-framework CoreFoundation"
    )
endif ()

# ---------------------------------------------------------------------------
# Install / export, so consumers can use find_package(NetLink) as well as CPM
# ---------------------------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS ${PROJECT_NAME}
        EXPORT NetLinkTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT NetLinkTargets
        FILE NetLinkTargets.cmake
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


# ---------------------------------------------------------------------------
# Tests and benchmarks
# ---------------------------------------------------------------------------

option(NETLINK_BUILD_TESTS "Build NetLink tests" ${PROJECT_IS_TOP_LEVEL})
option(NETLINK_BUILD_BENCHMARKS "Build NetLink benchmarks" ${PROJECT_IS_TOP_LEVEL})

if (NETLINK_BUILD_TESTS OR NETLINK_BUILD_BENCHMARKS)
    # Internal headers for in-tree consumers of the library
    add_library(NetLinkInternal INTERFACE)

    target_link_libraries(NetLinkInternal INTERFACE NetLink)

    target_include_directories(NetLinkInternal INTERFACE
            ${PROJECT_BINARY_DIR}/generated
            ${PROJECT_SOURCE_DIR}/src/internal
            ${PROJECT_SOURCE_DIR}/src/internal/Util
    )
endif ()

if (NETLINK_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif ()

if (NETLINK_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif ()
