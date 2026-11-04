# Developer tools.

add_executable(sedef-params sedef_params.cpp)
target_link_libraries(sedef-params PRIVATE sedef::engine sedef::compiler_settings)

add_executable(sedef-render sedef_render.cpp)
target_link_libraries(sedef-render PRIVATE sedef::engine sedef::compiler_settings)

if(SEDEF_BUILD_TESTS)
    # Every factory preset must render finite, unclipped and at a sane level.
    file(GLOB SEDEF_FACTORY_PRESET_FILES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/presets/factory/*.sedefpreset)
    add_test(NAME factory-presets COMMAND sedef-render --check ${SEDEF_FACTORY_PRESET_FILES})
    # Each one's board holds the pedals it switches on and sounds exactly like the fixed chain.
    add_test(NAME factory-preset-boards
        COMMAND sedef-render --check-boards ${SEDEF_FACTORY_PRESET_FILES})

    # The UI's copy of the schema must match the engine's.
    add_test(NAME ui-schema-in-sync
        COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:sedef-params>
                -DEXPECTED=${PROJECT_SOURCE_DIR}/ui/src/generated/parameters.json
                -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckSchema.cmake)
    # And its copy of the pedal registry.
    add_test(NAME ui-pedals-in-sync
        COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:sedef-params> -DARGS=--pedals
                -DEXPECTED=${PROJECT_SOURCE_DIR}/ui/src/generated/pedals.json
                -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckSchema.cmake)
endif()

if(SEDEF_BUILD_TESTS)
    # Transpose evaluation bench (docs/dsp/transpose.md). Not shipped.
    add_executable(sedef-transpose-bench
        transpose-bench/transpose_bench.cpp
        transpose-bench/LegacyWsola.cpp
        transpose-bench/SpectralShifter.cpp)
    target_include_directories(sedef-transpose-bench PRIVATE
        ${PROJECT_SOURCE_DIR}/engine/tests ${CMAKE_CURRENT_SOURCE_DIR}/transpose-bench)
    target_link_libraries(sedef-transpose-bench PRIVATE sedef::engine sedef::compiler_settings)

    # Optional third-party reference: Signalsmith Stretch (MIT), bench only.
    option(SEDEF_TRANSPOSE_BENCH_STRETCH "Compare against Signalsmith Stretch in the transpose bench" OFF)
    if(SEDEF_TRANSPOSE_BENCH_STRETCH)
        CPMAddPackage(
            NAME signalsmith_linear
            GITHUB_REPOSITORY Signalsmith-Audio/linear
            GIT_TAG 0.6.4
            DOWNLOAD_ONLY YES)
        CPMAddPackage(
            NAME signalsmith_stretch
            GITHUB_REPOSITORY Signalsmith-Audio/signalsmith-stretch
            GIT_TAG 1.4.0
            DOWNLOAD_ONLY YES)
        target_include_directories(sedef-transpose-bench SYSTEM PRIVATE
            ${signalsmith_stretch_SOURCE_DIR} ${signalsmith_linear_SOURCE_DIR}/include
            ${signalsmith_linear_SOURCE_DIR})
        target_compile_definitions(sedef-transpose-bench PRIVATE SEDEF_HAVE_STRETCH=1)
    endif()
endif()
