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
    target_sources(fm_protocol PRIVATE cpp/src/posix/UDPSocket.cpp)
endif()
target_sources(fm_protocol PRIVATE cpp/src/Fragmenter.cpp cpp/src/FrameReceiver.cpp cpp/src/Reassembler.cpp cpp/src/VideoSender.cpp)
target_include_directories(fm_protocol PUBLIC cpp/include)
target_link_libraries(fm_protocol PRIVATE fm_warnings)

if(ANDROID)
    set_target_properties(fm_protocol PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(freemonitor SHARED cpp/src/android/NativeBridge.cpp)
    target_link_libraries(freemonitor PRIVATE fm_protocol fm_warnings)
endif()

if(FM_BUILD_HOST)
    if(NOT WIN32)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
        pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
        pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)
    endif()

    add_library(fm_platform STATIC
            cpp/src/DisplayLayout.cpp
            cpp/src/stub/StubCaptureSource.cpp
            cpp/src/stub/StubEncoder.cpp
            cpp/src/stub/StubVirtualDisplay.cpp
            cpp/include/Reassembler.hpp
    )
    if(WIN32)
        target_sources(fm_platform PRIVATE cpp/src/stub/StubBackends.cpp)
    else()
        target_sources(fm_platform PRIVATE
                cpp/src/JPEGEncoder.cpp
                cpp/src/linux/HyprlandIPC.cpp
                cpp/src/linux/HyprlandVirtualDisplay.cpp
                cpp/src/linux/LinuxBackends.cpp
                cpp/src/linux/PipeWireCaptureSource.cpp
                cpp/src/linux/PortalScreenCast.cpp
                cpp/src/linux/RestoreTokenStore.cpp
        )
        target_link_libraries(fm_platform PRIVATE PkgConfig::PIPEWIRE PkgConfig::SYSTEMD PkgConfig::TURBOJPEG)
    endif()
    target_include_directories(fm_platform
            PUBLIC cpp/include
            PRIVATE cpp/src
    )
    target_link_libraries(fm_platform PRIVATE fm_warnings)

    add_library(fm_host STATIC cpp/src/FrameTimingLog.cpp cpp/src/HostOptions.cpp)
    target_include_directories(fm_host
            PUBLIC cpp/include cpp/src
    )
    target_compile_definitions(fm_host PUBLIC FM_DEVELOPMENT_MODE=$<IF:$<CONFIG:Release>,0,1>)
    target_link_libraries(fm_host PRIVATE fm_warnings)

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
    )
    if(NOT WIN32)
        target_sources(fm_tests PRIVATE
                cpp/tests/FileDescriptorTests.cpp
                cpp/tests/HyprlandIPCTests.cpp
                cpp/tests/HyprlandVirtualDisplayTests.cpp
                cpp/tests/JPEGEncoderTests.cpp
                cpp/tests/RestoreTokenStoreTests.cpp
                cpp/tests/PipeWireFormatTests.cpp
                cpp/tests/PortalScreenCastTests.cpp
        )
        target_link_libraries(fm_tests PRIVATE PkgConfig::PIPEWIRE PkgConfig::TURBOJPEG)
    endif()
    target_include_directories(fm_tests PRIVATE cpp/src)
    add_library(fm_test_warnings INTERFACE)
    target_compile_options(fm_test_warnings INTERFACE $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-c2y-extensions>)
    target_link_libraries(fm_tests PRIVATE fm_host fm_platform fm_protocol fm_warnings fm_test_warnings Catch2::Catch2WithMain)

    enable_testing()
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(fm_tests)
endif()
