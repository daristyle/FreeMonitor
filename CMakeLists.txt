cmake_minimum_required(VERSION 3.25)

project(FreeMonitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FM_BUILD_HOST "Build the host and its platform backends" ON)
option(FM_BUILD_TESTS "Build the Catch2 test suite" ${PROJECT_IS_TOP_LEVEL})
option(FM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

add_library(fm_warnings INTERFACE)
if(MSVC)
    target_compile_options(fm_warnings INTERFACE /W4 /permissive-)
    if(FM_WARNINGS_AS_ERRORS)
        target_compile_options(fm_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(fm_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    if(FM_WARNINGS_AS_ERRORS)
        target_compile_options(fm_warnings INTERFACE -Werror)
    endif()
endif()

if(FM_BUILD_TESTS AND NOT FM_BUILD_HOST)
    message(FATAL_ERROR "FM_BUILD_TESTS needs FM_BUILD_HOST")
endif()

add_library(fm_protocol STATIC)
if(WIN32)
    target_sources(fm_protocol PRIVATE cpp/src/windows/UDPSocket.cpp)
    target_compile_definitions(fm_protocol PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(fm_protocol PRIVATE ws2_32)
else()
    target_sources(fm_protocol PRIVATE cpp/src/posix/NetworkInterfaces.cpp cpp/src/posix/UDPSocket.cpp)
endif()
target_sources(fm_protocol PRIVATE
        cpp/src/ControlPackets.cpp
        cpp/src/CursorPackets.cpp
        cpp/src/CursorReceiver.cpp
        cpp/src/CursorShapeCache.cpp
        cpp/src/DiscoveryPackets.cpp
        cpp/src/EndpointParsing.cpp
        cpp/src/Fragmenter.cpp
        cpp/src/FrameReceiver.cpp
        cpp/src/HeartbeatEcho.cpp
        cpp/src/HostDiscovery.cpp
        cpp/src/HostSessions.cpp
        cpp/src/PeerTracker.cpp
        cpp/src/Reassembler.cpp
        cpp/src/ReedSolomon.cpp
        cpp/src/ReliableDelivery.cpp
        cpp/src/TabletSession.cpp
        cpp/src/UDPControlTransport.cpp
        cpp/src/VideoSender.cpp
)
target_include_directories(fm_protocol
        PUBLIC cpp/include
        PRIVATE cpp/src
)
target_link_libraries(fm_protocol PRIVATE fm_warnings)

if(ANDROID)
    set_target_properties(fm_protocol PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(freemonitor SHARED cpp/src/android/NativeBridge.cpp)
    target_include_directories(freemonitor PRIVATE cpp/src)
    target_link_libraries(freemonitor PRIVATE fm_protocol fm_warnings)
endif()

if(FM_BUILD_HOST)
    if(NOT WIN32)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
        pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
        pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)
        pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET libavcodec>=60 libavutil>=58)
        pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
        pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols>=1.37)
        pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
        pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)
        if(NOT WAYLAND_SCANNER)
            find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
        endif()

        enable_language(C)
        set(FM_WAYLAND_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/wayland)
        file(MAKE_DIRECTORY ${FM_WAYLAND_GENERATED_DIR})
        set(FM_WAYLAND_PROTOCOL_SOURCES)
        foreach(protocol ext-foreign-toplevel-list ext-image-capture-source ext-image-copy-capture)
            set(protocolXML ${WAYLAND_PROTOCOLS_DIR}/staging/${protocol}/${protocol}-v1.xml)
            set(protocolHeader ${FM_WAYLAND_GENERATED_DIR}/${protocol}-v1-client-protocol.h)
            set(protocolCode ${FM_WAYLAND_GENERATED_DIR}/${protocol}-v1-protocol.c)
            add_custom_command(OUTPUT ${protocolHeader}
                    COMMAND ${WAYLAND_SCANNER} client-header ${protocolXML} ${protocolHeader}
                    DEPENDS ${protocolXML} VERBATIM)
            add_custom_command(OUTPUT ${protocolCode}
                    COMMAND ${WAYLAND_SCANNER} private-code ${protocolXML} ${protocolCode}
                    DEPENDS ${protocolXML} VERBATIM)
            list(APPEND FM_WAYLAND_PROTOCOL_SOURCES ${protocolHeader} ${protocolCode})
        endforeach()
        add_library(fm_wayland_protocols STATIC ${FM_WAYLAND_PROTOCOL_SOURCES})
        target_include_directories(fm_wayland_protocols SYSTEM PUBLIC ${FM_WAYLAND_GENERATED_DIR})
        target_link_libraries(fm_wayland_protocols PUBLIC PkgConfig::WAYLAND_CLIENT)
    endif()

    add_library(fm_platform STATIC
            cpp/src/CursorTracker.cpp
            cpp/src/DisplayLayout.cpp
            cpp/src/FallbackEncoder.cpp
            cpp/src/H264Bitstream.cpp
            cpp/src/stub/StubCaptureSource.cpp
            cpp/src/stub/StubCursorSource.cpp
            cpp/src/stub/StubEncoder.cpp
            cpp/src/stub/StubVirtualDisplay.cpp
            cpp/include/Reassembler.hpp
    )
    if(WIN32)
        target_sources(fm_platform PRIVATE cpp/src/stub/StubBackends.cpp)
    else()
        target_sources(fm_platform PRIVATE
                cpp/src/JPEGEncoder.cpp
                cpp/src/LibavH264Encoder.cpp
                cpp/src/NVENCEncoder.cpp
                cpp/src/linux/HyprlandIPC.cpp
                cpp/src/linux/HyprlandVirtualDisplay.cpp
                cpp/src/linux/LinuxBackends.cpp
                cpp/src/linux/PipeWireCaptureSource.cpp
                cpp/src/linux/PortalScreenCast.cpp
                cpp/src/linux/RestoreTokenStore.cpp
                cpp/src/linux/VAAPIDevices.cpp
                cpp/src/linux/VAAPIEncoder.cpp
                cpp/src/linux/WaylandCursorSource.cpp
                cpp/src/linux/XCursorTheme.cpp
        )
        target_link_libraries(fm_platform PRIVATE PkgConfig::PIPEWIRE PkgConfig::SYSTEMD PkgConfig::TURBOJPEG
                PkgConfig::LIBAV fm_wayland_protocols)
    endif()
    target_include_directories(fm_platform
            PUBLIC cpp/include
            PRIVATE cpp/src
    )
    target_link_libraries(fm_platform PRIVATE fm_warnings)

    add_library(fm_host STATIC cpp/src/CursorSender.cpp cpp/src/DiscoveryResponder.cpp cpp/src/FrameTimingLog.cpp
            cpp/src/HostOptions.cpp cpp/src/RateLimiter.cpp)
    target_include_directories(fm_host
            PUBLIC cpp/include cpp/src
    )
    target_compile_definitions(fm_host PUBLIC FM_DEVELOPMENT_MODE=$<IF:$<CONFIG:Release>,0,1>)
    target_link_libraries(fm_host PRIVATE fm_protocol fm_warnings)

    add_executable(FreeMonitorHost cpp/src/HostMain.cpp)
    target_link_libraries(FreeMonitorHost PRIVATE fm_host fm_platform fm_protocol fm_warnings)
endif()

if(FM_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG v3.7.1
            GIT_SHALLOW TRUE
            SYSTEM
    )
    FetchContent_MakeAvailable(Catch2)

    add_executable(fm_tests
            cpp/tests/StubCaptureSourceTests.cpp
            cpp/tests/StubCursorSourceTests.cpp
            cpp/tests/CursorTrackerTests.cpp
            cpp/tests/CursorPacketsTests.cpp
            cpp/tests/CursorSenderTests.cpp
            cpp/tests/CursorReceiverTests.cpp
            cpp/tests/CursorShapeCacheTests.cpp
            cpp/tests/SequenceCounterTests.cpp
            cpp/tests/StubEncoderTests.cpp
            cpp/tests/StubVirtualDisplayTests.cpp
            cpp/tests/UDPSocketTests.cpp
            cpp/tests/FrameMailboxTests.cpp
            cpp/tests/SerialNumberTests.cpp
            cpp/tests/FragmenterTests.cpp
            cpp/tests/ReassemblerTests.cpp
            cpp/tests/PacketsTests.cpp
            cpp/tests/VideoSenderTests.cpp
            cpp/tests/FrameReceiverTests.cpp
            cpp/tests/FrameTimingLogTests.cpp
            cpp/tests/HostOptionsTests.cpp
            cpp/tests/DisplayLayoutTests.cpp
            cpp/tests/FallbackEncoderTests.cpp
            cpp/tests/H264BitstreamTests.cpp
            cpp/tests/ControlPacketsTests.cpp
            cpp/tests/ReliableDeliveryTests.cpp
            cpp/tests/PeerTrackerTests.cpp
            cpp/tests/HostSessionsTests.cpp
            cpp/tests/TabletSessionTests.cpp
            cpp/tests/ControlLoopbackTests.cpp
            cpp/tests/UDPControlTransportTests.cpp
            cpp/tests/EndpointParsingTests.cpp
            cpp/tests/HeartbeatEchoTests.cpp
            cpp/tests/ReedSolomonTests.cpp
            cpp/tests/DiscoveryPacketsTests.cpp
            cpp/tests/DiscoveryTests.cpp
            cpp/tests/RateLimiterTests.cpp
    )
    if(NOT WIN32)
        target_sources(fm_tests PRIVATE
                cpp/tests/FileDescriptorTests.cpp
                cpp/tests/NetworkInterfacesTests.cpp
                cpp/tests/HyprlandIPCTests.cpp
                cpp/tests/HyprlandVirtualDisplayTests.cpp
                cpp/tests/H264EncoderTests.cpp
                cpp/tests/JPEGEncoderTests.cpp
                cpp/tests/RestoreTokenStoreTests.cpp
                cpp/tests/PipeWireFormatTests.cpp
                cpp/tests/PortalScreenCastTests.cpp
                cpp/tests/VAAPIDevicesTests.cpp
                cpp/tests/UDPSocketTimestampTests.cpp
                cpp/tests/XCursorThemeTests.cpp
        )
        target_link_libraries(fm_tests PRIVATE PkgConfig::PIPEWIRE PkgConfig::TURBOJPEG PkgConfig::LIBAV)
    endif()
    target_include_directories(fm_tests PRIVATE cpp/src)
    add_library(fm_test_warnings INTERFACE)
    target_compile_options(fm_test_warnings INTERFACE $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-c2y-extensions>)
    target_link_libraries(fm_tests PRIVATE fm_host fm_platform fm_protocol fm_warnings fm_test_warnings Catch2::Catch2WithMain)

    add_executable(fm_conformance cpp/tests/ConformanceTool.cpp)
    target_link_libraries(fm_conformance PRIVATE fm_protocol fm_warnings)

    enable_testing()
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(fm_tests)
endif()
