###########################################
#   NetLink
#       by Jens W. Langenberg
###########################################

cmake_minimum_required(VERSION 4.0)

set(PROJECT_VERSION 0.6.0)
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
include(GNUInstallDirs)

file(GLOB_RECURSE NETWORKING_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB_RECURSE NETWORKING_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)
file(GLOB_RECURSE NETWORKING_INTERNAL_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h)

# The C API is a library of its own
list(FILTER NETWORKING_SOURCES EXCLUDE REGEX ".*/src/capi/.*")
list(FILTER NETWORKING_HEADERS EXCLUDE REGEX ".*/NetLinkC\.h$")

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

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
    target_compile_options(${PROJECT_NAME} PUBLIC $<BUILD_INTERFACE:-fexperimental-library>)
    target_link_options(${PROJECT_NAME} PUBLIC $<BUILD_INTERFACE:-fexperimental-library>)
endif ()

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
# C API: a shared library for hosts that cannot use the C++ API
# ---------------------------------------------------------------------------

option(NETLINK_BUILD_C_API "Build NetLinkC, the shared library with the plain C API" OFF)

if (NETLINK_BUILD_C_API)
    add_library(NetLinkC SHARED
            ${CMAKE_CURRENT_SOURCE_DIR}/src/capi/NetLinkC.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/NetLink/NetLinkC.h)
    add_library(NetLink::NetLinkC ALIAS NetLinkC)

    target_link_libraries(NetLinkC PRIVATE NetLink)
    target_compile_definitions(NetLinkC PRIVATE NETLINK_C_EXPORTS)
    target_include_directories(NetLinkC
            PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
            PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}/generated)

    set_target_properties(NetLinkC PROPERTIES
            VERSION ${PROJECT_VERSION}
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON)

    if (MSVC)
        target_compile_options(NetLinkC PRIVATE /W4)
    else ()
        target_compile_options(NetLinkC PRIVATE -Wall -Wextra -Wpedantic)
    endif ()

    foreach (flag IN LISTS NETLINK_SANITIZER_FLAGS)
        target_compile_options(NetLinkC PRIVATE ${flag} -fno-omit-frame-pointer)
        target_link_options(NetLinkC PRIVATE ${flag})
    endforeach ()
endif ()


# ---------------------------------------------------------------------------
# Install / export, so consumers can use find_package(NetLink) as well as CPM
# ---------------------------------------------------------------------------

include(CMakePackageConfigHelpers)

set(NETLINK_INSTALL_TARGETS ${PROJECT_NAME})

if (NETLINK_BUILD_C_API)
    list(APPEND NETLINK_INSTALL_TARGETS NetLinkC)
endif ()

install(TARGETS ${NETLINK_INSTALL_TARGETS}
        EXPORT NetLinkTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if (NETLINK_BUILD_C_API AND MSVC)
    install(FILES $<TARGET_PDB_FILE:NetLinkC> DESTINATION ${CMAKE_INSTALL_BINDIR} OPTIONAL)
endif ()

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
