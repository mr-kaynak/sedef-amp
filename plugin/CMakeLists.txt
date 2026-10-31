# The JUCE plugin shell (ADR-0002): VST3, AU, CLAP and Standalone around sedef::engine.

CPMAddPackage(
    NAME JUCE
    GITHUB_REPOSITORY juce-framework/JUCE
    GIT_TAG 9.0.4
    GIT_SHALLOW TRUE)

CPMAddPackage(
    NAME clap-juce-extensions
    GITHUB_REPOSITORY free-audio/clap-juce-extensions
    GIT_TAG 7adee3a1bd4684d4caa5601100e364abccff4b4d)

option(SEDEF_BUILD_UI "Build the web UI with pnpm as part of the build" ON)
option(SEDEF_COPY_PLUGIN "Copy plugins to the user plugin folders after building" OFF)
set(SEDEF_UI_DIR "${PROJECT_SOURCE_DIR}/ui")
set(SEDEF_UI_DIST "${SEDEF_UI_DIR}/dist")

# --- Web UI -> zip -> binary data ----------------------------------------------------------------

set(SEDEF_UI_ZIP "${CMAKE_CURRENT_BINARY_DIR}/sedef-ui.zip")

if(SEDEF_BUILD_UI)
    find_program(PNPM_EXECUTABLE pnpm REQUIRED)
    file(GLOB_RECURSE SEDEF_UI_SOURCES CONFIGURE_DEPENDS
        ${SEDEF_UI_DIR}/src/*
        ${SEDEF_UI_DIR}/public/*
        ${SEDEF_UI_DIR}/index.html
        ${SEDEF_UI_DIR}/package.json
        ${SEDEF_UI_DIR}/pnpm-lock.yaml
        ${SEDEF_UI_DIR}/vite.config.*)
    add_custom_command(
        OUTPUT "${SEDEF_UI_DIST}/index.html"
        COMMAND ${PNPM_EXECUTABLE} install --frozen-lockfile
        COMMAND ${PNPM_EXECUTABLE} run build
        WORKING_DIRECTORY ${SEDEF_UI_DIR}
        DEPENDS ${SEDEF_UI_SOURCES}
        COMMENT "Building the web UI"
        VERBATIM)
endif()

add_custom_command(
    OUTPUT ${SEDEF_UI_ZIP}
    COMMAND ${CMAKE_COMMAND} -E rm -f ${SEDEF_UI_ZIP}
    COMMAND ${CMAKE_COMMAND} -E tar cf ${SEDEF_UI_ZIP} --format=zip .
    WORKING_DIRECTORY ${SEDEF_UI_DIST}
    DEPENDS "${SEDEF_UI_DIST}/index.html"
    COMMENT "Packing the web UI"
    VERBATIM)

juce_add_binary_data(SedefUiData
    NAMESPACE SedefUiData
    HEADER_NAME SedefUiData.h
    SOURCES ${SEDEF_UI_ZIP})

file(GLOB SEDEF_FACTORY_PRESETS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/presets/factory/*.sedefpreset)
juce_add_binary_data(SedefPresetData
    NAMESPACE SedefPresetData
    HEADER_NAME SedefPresetData.h
    SOURCES ${SEDEF_FACTORY_PRESETS})

# --- Plugin --------------------------------------------------------------------------------------

juce_add_plugin(SedefAmp
    PRODUCT_NAME "Sedef Amp"
    COMPANY_NAME "Sedef"
    COMPANY_WEBSITE "https://github.com/mr-kaynak/sedef-amp"
    BUNDLE_ID "io.github.mr-kaynak.sedef-amp"
    VERSION ${PROJECT_VERSION}
    PLUGIN_MANUFACTURER_CODE Sedf
    PLUGIN_CODE Sda1
    FORMATS AU VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    NEEDS_WEB_BROWSER TRUE
    NEEDS_WEBVIEW2 TRUE
    VST3_CATEGORIES Fx Distortion
    AU_MAIN_TYPE kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD ${SEDEF_COPY_PLUGIN}
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Sedef Amp processes your guitar's audio input.")

clap_juce_extensions_plugin(TARGET SedefAmp
    CLAP_ID "io.github.mr-kaynak.sedef-amp"
    CLAP_FEATURES audio-effect distortion mono stereo)

file(GLOB SEDEF_PLUGIN_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h)
target_sources(SedefAmp PRIVATE ${SEDEF_PLUGIN_SOURCES})

target_compile_definitions(SedefAmp PUBLIC
    JUCE_WEB_BROWSER=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    SEDEF_VERSION="${PROJECT_VERSION}")

target_link_libraries(SedefAmp
    PRIVATE
        SedefUiData
        SedefPresetData
        sedef::engine
        juce::juce_audio_utils
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
