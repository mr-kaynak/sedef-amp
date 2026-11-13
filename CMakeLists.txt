cmake_minimum_required(VERSION 3.25)

project(SedefAmp
    VERSION 0.10.1
    DESCRIPTION "Open-source guitar amp and effects suite"
    HOMEPAGE_URL "https://github.com/mr-kaynak/sedef-amp"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

option(SEDEF_BUILD_PLUGIN "Build the JUCE plugin and standalone app" ON)
option(SEDEF_BUILD_TESTS "Build engine tests" ON)
option(SEDEF_BUILD_TOOLS "Build developer tools" ON)
option(SEDEF_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(SEDEF_SANITIZE_THREAD "Build with ThreadSanitizer" OFF)
option(SEDEF_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CPM)
include(SedefCompilerSettings)

if(SEDEF_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(engine)

if(SEDEF_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(SEDEF_BUILD_PLUGIN)
    add_subdirectory(plugin)
endif()
