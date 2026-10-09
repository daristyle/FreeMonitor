cmake_minimum_required(VERSION 3.25)

project(FreeMonitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

add_library(fm_platform STATIC
        cpp/src/stub/StubCaptureSource.cpp
        cpp/src/stub/StubEncoder.cpp
        cpp/src/stub/StubVirtualDisplay.cpp
        cpp/src/stub/StubBackends.cpp
)
target_include_directories(fm_platform
        PUBLIC cpp/include
        PRIVATE cpp/src
)
target_link_libraries(fm_platform PRIVATE fm_warnings)

add_library(fm_protocol STATIC)
if(WIN32)
    target_sources(fm_protocol PRIVATE cpp/src/windows/UDPSocket.cpp)
    target_compile_definitions(fm_protocol PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(fm_protocol PRIVATE ws2_32)
else()
    target_sources(fm_protocol PRIVATE cpp/src/posix/UDPSocket.cpp)
endif()
target_include_directories(fm_protocol PUBLIC cpp/include)
target_link_libraries(fm_protocol PRIVATE fm_warnings)

add_executable(FreeMonitorHost cpp/src/HostMain.cpp)
target_link_libraries(FreeMonitorHost PRIVATE fm_platform fm_warnings)

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
    )
    target_include_directories(fm_tests PRIVATE cpp/src)
    add_library(fm_test_warnings INTERFACE)
    target_compile_options(fm_test_warnings INTERFACE $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-c2y-extensions>)
    target_link_libraries(fm_tests PRIVATE fm_platform fm_protocol fm_warnings fm_test_warnings Catch2::Catch2WithMain)

    enable_testing()
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(fm_tests)
endif()
