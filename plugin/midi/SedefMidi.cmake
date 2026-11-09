# Sedef MIDI (docs/design/guitar-to-midi.md): a guitar's DI to MIDI notes. An audio effect (AU,
# VST3, CLAP and a standalone app with its own virtual MIDI port) and, on macOS, an instrument AU
# that takes the guitar through a side chain, so Logic's instrument tracks can record its notes.
# Included from plugin/CMakeLists.txt, so it shares the web UI's build rules.

# The product's identity, in one place. Frozen since 0.6.0, its first release: hosts, sessions and
# installers know Sedef MIDI by these (docs/midi.md#identity). Never change them, nor the AU types
# below (aufx for the effect, aumu for the instrument).
set(SEDEF_MIDI_EFFECT_CODE Sdm1)
set(SEDEF_MIDI_INSTRUMENT_CODE Sdm2)
set(SEDEF_MIDI_BUNDLE_ID "io.github.mr-kaynak.sedef-midi")
set(SEDEF_MIDI_INSTRUMENT_BUNDLE_ID "io.github.mr-kaynak.sedef-midi-instrument")
set(SEDEF_MIDI_CLAP_ID "io.github.mr-kaynak.sedef-midi")

# --- Web UI (ui/midi.html -> ui/dist-midi) -> zip -> binary data --------------------------------

set(SEDEF_MIDI_UI_DIST "${SEDEF_UI_DIR}/dist-midi")
set(SEDEF_MIDI_UI_ZIP "${CMAKE_CURRENT_BINARY_DIR}/sedef-midi-ui.zip")

if(SEDEF_BUILD_UI)
    add_custom_command(
        OUTPUT "${SEDEF_MIDI_UI_DIST}/midi.html"
        COMMAND ${PNPM_EXECUTABLE} run build:midi
        WORKING_DIRECTORY ${SEDEF_UI_DIR}
        DEPENDS ${SEDEF_UI_INSTALL_STAMP} ${SEDEF_UI_SOURCES} ${SEDEF_UI_DIR}/midi.html
        COMMENT "Building the Sedef MIDI web UI"
        VERBATIM)
endif()

add_custom_command(
    OUTPUT ${SEDEF_MIDI_UI_ZIP}
    COMMAND ${CMAKE_COMMAND} -E rm -f ${SEDEF_MIDI_UI_ZIP}
    COMMAND ${CMAKE_COMMAND} -E tar cf ${SEDEF_MIDI_UI_ZIP} --format=zip .
    WORKING_DIRECTORY ${SEDEF_MIDI_UI_DIST}
    DEPENDS "${SEDEF_MIDI_UI_DIST}/midi.html"
    COMMENT "Packing the Sedef MIDI web UI"
    VERBATIM)

juce_add_binary_data(SedefMidiUiData
    NAMESPACE SedefMidiUiData
    HEADER_NAME SedefMidiUiData.h
    SOURCES ${SEDEF_MIDI_UI_ZIP})

# --- Plug-ins ------------------------------------------------------------------------------------

set(SEDEF_MIDI_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/midi/MidiProcessor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/midi/MidiProcessor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/midi/MidiEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/midi/MidiEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/midi/MidiPortRouter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/midi/MidiPortRouter.h
    ${CMAKE_CURRENT_SOURCE_DIR}/midi/MidiProduct.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ParameterLayout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ParameterLayout.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ZipResources.h)

function(sedef_midi_configure target instrument)
    target_sources(${target} PRIVATE ${SEDEF_MIDI_SOURCES})
    target_compile_definitions(${target} PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
        SEDEF_MIDI_INSTRUMENT=${instrument}
        SEDEF_VERSION="${PROJECT_VERSION}")
    target_link_libraries(${target}
        PRIVATE
            SedefMidiUiData
            sedef::engine
            juce::juce_audio_utils
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endfunction()

juce_add_plugin(SedefMidi
    PRODUCT_NAME "Sedef MIDI"
    COMPANY_NAME "Sedef"
    COMPANY_WEBSITE "https://github.com/mr-kaynak/sedef-amp"
    BUNDLE_ID "${SEDEF_MIDI_BUNDLE_ID}"
    ICON_BIG "${PROJECT_SOURCE_DIR}/tools/packaging/assets/icon-1024.png"
    VERSION ${PROJECT_VERSION}
    PLUGIN_MANUFACTURER_CODE Sedf
    PLUGIN_CODE ${SEDEF_MIDI_EFFECT_CODE}
    DESCRIPTION "Guitar to MIDI"
    FORMATS AU VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    NEEDS_WEB_BROWSER TRUE
    NEEDS_WEBVIEW2 TRUE
    NEEDS_CURL FALSE
    VST3_CATEGORIES Fx Analyzer
    AU_MAIN_TYPE kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD ${SEDEF_COPY_PLUGIN}
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Sedef MIDI turns your guitar's audio input into MIDI notes.")

clap_juce_extensions_plugin(TARGET SedefMidi
    CLAP_ID "${SEDEF_MIDI_CLAP_ID}"
    CLAP_FEATURES audio-effect note-detector analyzer mono)

sedef_midi_configure(SedefMidi 0)

# Logic records MIDI from instrument tracks only: an instrument AU (aumu) whose input is a side
# chain, so the guitar track feeds it and other instrument tracks take its notes ("MIDI From").
if(APPLE)
    juce_add_plugin(SedefMidiInstrument
        PRODUCT_NAME "Sedef MIDI Instrument"
        PLUGIN_NAME "Sedef MIDI (Instrument)"
        COMPANY_NAME "Sedef"
        COMPANY_WEBSITE "https://github.com/mr-kaynak/sedef-amp"
        BUNDLE_ID "${SEDEF_MIDI_INSTRUMENT_BUNDLE_ID}"
        VERSION ${PROJECT_VERSION}
        PLUGIN_MANUFACTURER_CODE Sedf
        PLUGIN_CODE ${SEDEF_MIDI_INSTRUMENT_CODE}
        DESCRIPTION "Guitar to MIDI, for an instrument track's side chain"
        FORMATS AU
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        NEEDS_WEB_BROWSER TRUE
        NEEDS_CURL FALSE
        AU_MAIN_TYPE kAudioUnitType_MusicDevice
        COPY_PLUGIN_AFTER_BUILD ${SEDEF_COPY_PLUGIN})
    sedef_midi_configure(SedefMidiInstrument 1)
endif()

# --- Tests ---------------------------------------------------------------------------------------
# The port router through a real virtual port, the parameters' text, and the built VST3 hosted
# the way a DAW hosts it (its MIDI output against the engine's, bypass, transport, state).

if(SEDEF_BUILD_PLUGIN_TESTS)
    CPMAddPackage(
        NAME Catch2
        GITHUB_REPOSITORY catchorg/Catch2
        VERSION 3.16.1)

    juce_add_console_app(sedef_midi_tests PRODUCT_NAME "sedef_midi_tests")
    target_sources(sedef_midi_tests PRIVATE
        midi/tests/MidiClapTests.cpp
        midi/tests/MidiHostTests.cpp
        midi/tests/MidiPortTests.cpp
        midi/tests/MidiTestMain.cpp
        midi/MidiPortRouter.cpp
        src/ParameterLayout.cpp)
    target_include_directories(sedef_midi_tests PRIVATE
        midi src ${PROJECT_SOURCE_DIR}/engine/tests)
    target_compile_definitions(sedef_midi_tests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_PLUGINHOST_VST3=1
        JUCE_MODAL_LOOPS_PERMITTED=1
        SEDEF_MIDI_VST3_PATH="${CMAKE_CURRENT_BINARY_DIR}/SedefMidi_artefacts/$<CONFIG>/VST3/Sedef MIDI.vst3"
        SEDEF_MIDI_CLAP_PATH="${CMAKE_CURRENT_BINARY_DIR}/SedefMidi_artefacts/$<CONFIG>/CLAP/Sedef MIDI.clap")
    target_link_libraries(sedef_midi_tests
        PRIVATE
            sedef::engine
            juce::juce_audio_devices
            juce::juce_audio_processors
            juce::juce_gui_basics
            Catch2::Catch2
            clap
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
    add_dependencies(sedef_midi_tests SedefMidi_VST3 SedefMidi_CLAP)
    # The engine's test signals compare floats exactly on purpose.
    if(NOT MSVC)
        target_compile_options(sedef_midi_tests PRIVATE -Wno-float-equal)
    endif()

    list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(sedef_midi_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "midi plug-in: ")
endif()

if(TARGET sedef-host-smoke)
    add_dependencies(sedef-host-smoke SedefMidi_VST3)
endif()
