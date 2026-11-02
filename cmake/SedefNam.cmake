# Neural Amp Modeler core (MIT) and its dependencies Eigen (MPL-2.0) and nlohmann/json (MIT), for
# loading .nam captures (docs/dsp/nam.md). All three are pinned release archives with their hashes.
#
# The core is built as its own static library with warnings off: it is third-party code, and the
# engine's warnings-as-errors flags (sedef::compiler_settings) must not apply to it. Its headers,
# Eigen's and json.hpp are SYSTEM includes so they don't warn inside Sedef's own sources either.

CPMAddPackage(
    NAME NeuralAmpModelerCore
    URL https://github.com/sdatkinson/NeuralAmpModelerCore/archive/refs/tags/v0.6.0.tar.gz
    URL_HASH SHA256=d55f3c49278a06e5bf5978ea60ae8384603b843d7fdee22a0e9ba71c95108450
    DOWNLOAD_ONLY YES)

# The release NeuralAmpModelerCore v0.6.0 pins as its submodule (Eigen 5.0.1).
CPMAddPackage(
    NAME Eigen
    URL https://gitlab.com/libeigen/eigen/-/archive/5.0.1/eigen-5.0.1.tar.gz
    URL_HASH SHA256=e9c326dc8c05cd1e044c71f30f1b2e34a6161a3b6ecf445d56b53ff1669e3dec
    DOWNLOAD_ONLY YES)

# The version the core vendors (json.hpp 3.12.0); the release asset holds just the headers.
CPMAddPackage(
    NAME nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_ONLY YES)

file(GLOB SEDEF_NAM_SOURCES CONFIGURE_DEPENDS
    ${NeuralAmpModelerCore_SOURCE_DIR}/NAM/*.cpp
    ${NeuralAmpModelerCore_SOURCE_DIR}/NAM/wavenet/*.cpp)

add_library(sedef_nam STATIC ${SEDEF_NAM_SOURCES})
target_include_directories(sedef_nam SYSTEM PUBLIC
    ${NeuralAmpModelerCore_SOURCE_DIR}/NAM
    ${NeuralAmpModelerCore_SOURCE_DIR}
    ${Eigen_SOURCE_DIR}
    ${nlohmann_json_SOURCE_DIR}/single_include/nlohmann)
target_compile_features(sedef_nam PUBLIC cxx_std_20)
# NAM_SAMPLE_FLOAT changes class layouts, so every user of the headers must see it.
target_compile_definitions(sedef_nam
    PUBLIC NAM_SAMPLE_FLOAT
    PRIVATE NAM_ENABLE_A2_FAST $<$<PLATFORM_ID:Windows>:NOMINMAX WIN32_LEAN_AND_MEAN>)
if(MSVC)
    target_compile_options(sedef_nam PRIVATE /W0 /bigobj /utf-8 /permissive-)
else()
    target_compile_options(sedef_nam PRIVATE -w $<$<CONFIG:Release,RelWithDebInfo>:-O3>)
endif()
if(SEDEF_SANITIZE AND NOT MSVC)
    target_compile_options(sedef_nam PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
endif()
if(SEDEF_SANITIZE_THREAD AND NOT MSVC)
    target_compile_options(sedef_nam PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
endif()
