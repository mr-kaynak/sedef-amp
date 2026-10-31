# The Sedef DSP engine: pure C++20, no JUCE (ADR-0001).

CPMAddPackage(
    NAME pffft
    GITHUB_REPOSITORY marton78/pffft
    GIT_TAG v1.1.0
    DOWNLOAD_ONLY YES)

add_library(sedef_pffft STATIC ${pffft_SOURCE_DIR}/src/pffft.c ${pffft_SOURCE_DIR}/src/pffft_common.c)
target_include_directories(sedef_pffft SYSTEM PUBLIC ${pffft_SOURCE_DIR}/include)
target_include_directories(sedef_pffft PRIVATE ${pffft_SOURCE_DIR}/src ${pffft_SOURCE_DIR}/include/pffft)
target_compile_definitions(sedef_pffft PRIVATE _USE_MATH_DEFINES)
if(NOT MSVC)
    target_compile_options(sedef_pffft PRIVATE -w)
endif()

# Sources are globbed so each module can be added without touching this file.
file(GLOB_RECURSE SEDEF_ENGINE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB_RECURSE SEDEF_ENGINE_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.inc)

add_library(sedef_engine STATIC ${SEDEF_ENGINE_SOURCES} ${SEDEF_ENGINE_HEADERS})
add_library(sedef::engine ALIAS sedef_engine)
target_include_directories(sedef_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sedef_engine
    PUBLIC sedef_pffft
    PRIVATE sedef::compiler_settings)
target_compile_features(sedef_engine PUBLIC cxx_std_20)

if(SEDEF_BUILD_TESTS)
    CPMAddPackage(
        NAME Catch2
        GITHUB_REPOSITORY catchorg/Catch2
        VERSION 3.16.1)

    file(GLOB_RECURSE SEDEF_ENGINE_TESTS CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)

    add_executable(sedef_engine_tests ${SEDEF_ENGINE_TESTS})
    target_include_directories(sedef_engine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(sedef_engine_tests
        PRIVATE sedef::engine Catch2::Catch2WithMain sedef::compiler_settings)

    enable_testing()
    list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(sedef_engine_tests DISCOVERY_MODE PRE_TEST)
endif()
