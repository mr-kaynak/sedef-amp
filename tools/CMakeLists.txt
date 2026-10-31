# Developer tools.

add_executable(sedef-params sedef_params.cpp)
target_link_libraries(sedef-params PRIVATE sedef::engine sedef::compiler_settings)

add_executable(sedef-render sedef_render.cpp)
target_link_libraries(sedef-render PRIVATE sedef::engine sedef::compiler_settings)

if(SEDEF_BUILD_TESTS)
    # Every factory preset must render finite, unclipped and at a sane level.
    file(GLOB SEDEF_FACTORY_PRESET_FILES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/presets/factory/*.sedefpreset)
    add_test(NAME factory-presets COMMAND sedef-render --check ${SEDEF_FACTORY_PRESET_FILES})

    # The UI's copy of the schema must match the engine's.
    add_test(NAME ui-schema-in-sync
        COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:sedef-params>
                -DEXPECTED=${PROJECT_SOURCE_DIR}/ui/src/generated/parameters.json
                -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckSchema.cmake)
endif()
