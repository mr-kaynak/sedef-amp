# The JUCE plugin shell (ADR-0002): VST3, AU, CLAP and Standalone around sedef::engine.

CPMAddPackage(
    NAME JUCE
    GITHUB_REPOSITORY juce-framework/JUCE
    GIT_TAG 9.0.4
    GIT_SHALLOW TRUE)

# The patch stops the wrapper from pairing a mono input with a stereo output for in-place
# processing (clap-validator: process-audio-basic-in-place).
CPMAddPackage(
    NAME clap-juce-extensions
    GITHUB_REPOSITORY free-audio/clap-juce-extensions
    GIT_TAG 7adee3a1bd4684d4caa5601100e364abccff4b4d
    PATCHES ${PROJECT_SOURCE_DIR}/cmake/patches/clap-juce-extensions-in-place-pair.patch)

option(SEDEF_BUILD_UI "Build the web UI with pnpm as part of the build" ON)
option(SEDEF_COPY_PLUGIN "Copy plugins to the user plugin folders after building" OFF)
option(SEDEF_UPDATE_CHECK "Build the in-app update checker (talks to api.github.com, docs/updates.md)" ON)
option(SEDEF_BUILD_PLUGIN_TESTS "Build the plug-in's unit tests (update checker)" ${SEDEF_BUILD_TESTS})

# JUCE has no HTTPS on Linux without libcurl. It is loaded when the first request runs, so the
# plug-in still loads on systems without it (the check then reports a network error).
if(SEDEF_UPDATE_CHECK AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(SEDEF_NEEDS_CURL TRUE)
else()
    set(SEDEF_NEEDS_CURL FALSE)
endif()
set(SEDEF_UI_DIR "${PROJECT_SOURCE_DIR}/ui")
set(SEDEF_UI_DIST "${SEDEF_UI_DIR}/dist")

# --- Web UI -> zip -> binary data ----------------------------------------------------------------

set(SEDEF_UI_ZIP "${CMAKE_CURRENT_BINARY_DIR}/sedef-ui.zip")

if(SEDEF_BUILD_UI)
    find_program(PNPM_EXECUTABLE pnpm REQUIRED)
    # Install only when the manifest or lockfile changes; rebuild the bundle when sources change.
    set(SEDEF_UI_INSTALL_STAMP "${CMAKE_CURRENT_BINARY_DIR}/ui-install.stamp")
    add_custom_command(
        OUTPUT ${SEDEF_UI_INSTALL_STAMP}
        COMMAND ${PNPM_EXECUTABLE} install --frozen-lockfile
        COMMAND ${CMAKE_COMMAND} -E touch ${SEDEF_UI_INSTALL_STAMP}
        WORKING_DIRECTORY ${SEDEF_UI_DIR}
        DEPENDS ${SEDEF_UI_DIR}/package.json ${SEDEF_UI_DIR}/pnpm-lock.yaml
        COMMENT "Installing web UI dependencies"
        VERBATIM)
    # GLOB_RECURSE matches a pattern in every subfolder, so the top-level files get a plain GLOB
    # (otherwise ui/dist/index.html would depend on itself).
    file(GLOB_RECURSE SEDEF_UI_SOURCES CONFIGURE_DEPENDS
        ${SEDEF_UI_DIR}/src/*
        ${SEDEF_UI_DIR}/public/*)
    file(GLOB SEDEF_UI_TOP_LEVEL CONFIGURE_DEPENDS
        ${SEDEF_UI_DIR}/index.html
        ${SEDEF_UI_DIR}/vite.config.*
        ${SEDEF_UI_DIR}/tsconfig*.json)
    list(APPEND SEDEF_UI_SOURCES ${SEDEF_UI_TOP_LEVEL})
    add_custom_command(
        OUTPUT "${SEDEF_UI_DIST}/index.html"
        COMMAND ${PNPM_EXECUTABLE} run build
        WORKING_DIRECTORY ${SEDEF_UI_DIR}
        DEPENDS ${SEDEF_UI_INSTALL_STAMP} ${SEDEF_UI_SOURCES}
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
if(NOT SEDEF_FACTORY_PRESETS)
    message(FATAL_ERROR "No factory presets found in presets/factory")
endif()
juce_add_binary_data(SedefPresetData
    NAMESPACE SedefPresetData
    HEADER_NAME SedefPresetData.h
    SOURCES ${SEDEF_FACTORY_PRESETS})

# --- WebView2 (Windows) --------------------------------------------------------------------------
# JUCE links the WebView2 loader statically; fetch the NuGet package where JUCE looks for it.
if(WIN32 AND NOT JUCE_WEBVIEW2_PACKAGE_LOCATION)
    set(SEDEF_WEBVIEW2_VERSION 1.0.2903.40)
    set(SEDEF_WEBVIEW2_ROOT "${CMAKE_BINARY_DIR}/webview2")
    set(SEDEF_WEBVIEW2_DIR "${SEDEF_WEBVIEW2_ROOT}/Microsoft.Web.WebView2.${SEDEF_WEBVIEW2_VERSION}")
    if(NOT EXISTS "${SEDEF_WEBVIEW2_DIR}/build/native/include/WebView2.h")
        file(DOWNLOAD
            "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${SEDEF_WEBVIEW2_VERSION}"
            "${SEDEF_WEBVIEW2_ROOT}/webview2.nupkg" STATUS dl_status)
        file(ARCHIVE_EXTRACT INPUT "${SEDEF_WEBVIEW2_ROOT}/webview2.nupkg" DESTINATION "${SEDEF_WEBVIEW2_DIR}")
    endif()
    set(JUCE_WEBVIEW2_PACKAGE_LOCATION "${SEDEF_WEBVIEW2_ROOT}" CACHE PATH "WebView2 NuGet packages" FORCE)
endif()

# --- Plugin --------------------------------------------------------------------------------------

juce_add_plugin(SedefAmp
    PRODUCT_NAME "Sedef Amp"
    COMPANY_NAME "Sedef"
    COMPANY_WEBSITE "https://github.com/mr-kaynak/sedef-amp"
    BUNDLE_ID "io.github.mr-kaynak.sedef-amp"
    ICON_BIG "${PROJECT_SOURCE_DIR}/tools/packaging/assets/icon-1024.png"
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
    NEEDS_CURL ${SEDEF_NEEDS_CURL}
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

# Update checker (docs/updates.md). The bridge is always built: with the option off it reports
# the feature as unsupported and the UI hides it.
set(SEDEF_UPDATE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/update)
set(SEDEF_UPDATE_CORE_SOURCES
    ${SEDEF_UPDATE_DIR}/UpdateLogic.cpp ${SEDEF_UPDATE_DIR}/UpdateLogic.h
    ${SEDEF_UPDATE_DIR}/UpdateClient.cpp ${SEDEF_UPDATE_DIR}/UpdateClient.h
    ${SEDEF_UPDATE_DIR}/DownloadMark.cpp ${SEDEF_UPDATE_DIR}/DownloadMark.h
    ${SEDEF_UPDATE_DIR}/InstallLogic.cpp ${SEDEF_UPDATE_DIR}/InstallLogic.h
    ${SEDEF_UPDATE_DIR}/InstallProcess.cpp ${SEDEF_UPDATE_DIR}/InstallProcess.h
    ${SEDEF_UPDATE_DIR}/Installer.cpp ${SEDEF_UPDATE_DIR}/Installer.h)
target_sources(SedefAmp PRIVATE ${SEDEF_UPDATE_DIR}/UpdateBridge.cpp ${SEDEF_UPDATE_DIR}/UpdateBridge.h)
if(SEDEF_UPDATE_CHECK)
    target_sources(SedefAmp PRIVATE ${SEDEF_UPDATE_CORE_SOURCES}
        ${SEDEF_UPDATE_DIR}/UpdateService.cpp ${SEDEF_UPDATE_DIR}/UpdateService.h)
    target_link_libraries(SedefAmp PRIVATE juce::juce_cryptography)
endif()

target_compile_definitions(SedefAmp PUBLIC
    JUCE_WEB_BROWSER=1
    JUCE_USE_CURL=$<BOOL:${SEDEF_NEEDS_CURL}>
    JUCE_LOAD_CURL_SYMBOLS_LAZILY=1
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    SEDEF_UPDATE_CHECK=$<BOOL:${SEDEF_UPDATE_CHECK}>
    SEDEF_VERSION="${PROJECT_VERSION}")

target_link_libraries(SedefAmp
    PRIVATE
        SedefUiData
        SedefPresetData
        sedef::engine
        juce::juce_audio_utils
        juce::juce_cryptography
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# --- Host smoke test -----------------------------------------------------------------------------
# A minimal host that opens the VST3's editor and waits for its UI smoke test (tools/ci/ui-smoke.sh).

option(SEDEF_BUILD_HOST_SMOKE "Build sedef-host-smoke, a minimal VST3 host for UI smoke tests" OFF)
if(SEDEF_BUILD_HOST_SMOKE)
    juce_add_gui_app(sedef-host-smoke
        PRODUCT_NAME "sedef-host-smoke"
        COMPANY_NAME "Sedef"
        BUNDLE_ID "io.github.mr-kaynak.sedef-host-smoke"
        VERSION ${PROJECT_VERSION})
    target_sources(sedef-host-smoke PRIVATE smoke/HostSmoke.cpp)
    target_compile_definitions(sedef-host-smoke PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_APPLICATION_NAME_STRING="sedef-host-smoke"
        JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")
    target_link_libraries(sedef-host-smoke PRIVATE
        juce::juce_audio_processors
        juce::juce_gui_basics
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    add_dependencies(sedef-host-smoke SedefAmp_VST3)
endif()

# --- Update checker: unit tests and a command-line check ----------------------------------------
# Both compile the checker's sources against juce_core, without the plug-in around them.

if(SEDEF_UPDATE_CHECK AND SEDEF_BUILD_PLUGIN_TESTS)
    CPMAddPackage(
        NAME Catch2
        GITHUB_REPOSITORY catchorg/Catch2
        VERSION 3.16.1)

    juce_add_console_app(sedef_update_tests PRODUCT_NAME "sedef_update_tests")
    target_sources(sedef_update_tests PRIVATE
        tests/UpdateTests.cpp tests/UpdateClientTests.cpp tests/UpdateServiceTests.cpp
        tests/InstallTests.cpp
        ${SEDEF_UPDATE_CORE_SOURCES} ${SEDEF_UPDATE_DIR}/UpdateService.cpp)
    target_include_directories(sedef_update_tests PRIVATE ${SEDEF_UPDATE_DIR} tests)
    target_compile_definitions(sedef_update_tests PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 SEDEF_VERSION="0.1.0")
    target_link_libraries(sedef_update_tests
        PRIVATE
            juce::juce_core
            juce::juce_cryptography
            juce::juce_events
            Catch2::Catch2WithMain
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(sedef_update_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "update: ")
endif()

# sedef-update-check: what the plug-in's checker sees, against the real GitHub API.
if(SEDEF_UPDATE_CHECK AND SEDEF_BUILD_TOOLS)
    juce_add_console_app(sedef-update-check
        PRODUCT_NAME "sedef-update-check"
        NEEDS_CURL ${SEDEF_NEEDS_CURL})
    target_sources(sedef-update-check PRIVATE tools/sedef_update_check.cpp ${SEDEF_UPDATE_CORE_SOURCES})
    target_include_directories(sedef-update-check PRIVATE ${SEDEF_UPDATE_DIR})
    target_compile_definitions(sedef-update-check PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=$<BOOL:${SEDEF_NEEDS_CURL}>
        JUCE_LOAD_CURL_SYMBOLS_LAZILY=1
        SEDEF_VERSION="${PROJECT_VERSION}")
    target_link_libraries(sedef-update-check
        PRIVATE
            juce::juce_core
            juce::juce_cryptography
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()

# --- Capture library: unit tests ------------------------------------------------------------------
# CaptureLibrary (session state, preset references, recent list) against the real engine.

if(SEDEF_BUILD_PLUGIN_TESTS)
    CPMAddPackage(
        NAME Catch2
        GITHUB_REPOSITORY catchorg/Catch2
        VERSION 3.16.1)

    juce_add_console_app(sedef_capture_tests PRODUCT_NAME "sedef_capture_tests")
    target_sources(sedef_capture_tests PRIVATE
        tests/CaptureLibraryTests.cpp src/CaptureLibrary.cpp src/CaptureLibrary.h
        tests/IrLibraryTests.cpp src/IrLibrary.cpp src/IrLibrary.h)
    target_include_directories(sedef_capture_tests PRIVATE src)
    target_compile_definitions(sedef_capture_tests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(sedef_capture_tests
        PRIVATE
            sedef::engine
            juce::juce_audio_formats
            juce::juce_core
            juce::juce_cryptography
            juce::juce_data_structures
            juce::juce_events
            Catch2::Catch2WithMain
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(sedef_capture_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "capture library: ")
endif()

# --- Sedef MIDI ------------------------------------------------------------------------------------

include(${CMAKE_CURRENT_SOURCE_DIR}/midi/SedefMidi.cmake)
