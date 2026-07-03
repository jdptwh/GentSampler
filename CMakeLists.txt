cmake_minimum_required(VERSION 3.22)
project(GentSampler VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ---- JUCE framework ----
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# ---- Signalsmith Stretch (time-stretch / pitch-shift engine, MIT) ----
FetchContent_Declare(signalsmith_stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG        main)
FetchContent_MakeAvailable(signalsmith_stretch)

# ============================================================================
#  ONNX Runtime (GPU build: CUDA + TensorRT + CPU) for the stem-separation engine.
#  Auto-downloaded as a NuGet package (.nupkg = zip) so build.bat stays one-click.
#  The execution provider is chosen at RUNTIME by the provider-priority chain in
#  StemSeparator (CUDA -> CPU in v1). The heavy CUDA/cuDNN runtime DLLs and the
#  large CUDA EP DLL are NOT bundled here -- they are a first-run download (see
#  ModelDownloader) so the installer stays small for CPU-only users.
# ============================================================================
# Legacy ORT 1.18.1 (CUDA 11.8 + cuDNN 8.9 build). The modern 1.22 + cuDNN-9 path is
# a dead end on this model/GPU (cuDNN-9 conv frontend wall + a 1.21/1.22 CUDA stream
# regression that errors at Add nodes and can crash the host). 1.18's legacy cuDNN-8
# conv path runs the FULL 7.8s model cleanly on the RTX 2060 (de-risked in Python).
set(ORT_VERSION "1.18.1")
FetchContent_Declare(onnxruntime_gpu
    URL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-win-x64-gpu-${ORT_VERSION}.zip"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(onnxruntime_gpu)

# The GitHub zip extracts to a top-level onnxruntime-win-x64-gpu-<ver>/ with include/
# and lib/ subdirs. Locate them by finding the header (robust to any flattening).
file(GLOB_RECURSE _ORT_HDR "${onnxruntime_gpu_SOURCE_DIR}/onnxruntime_cxx_api.h")
if(_ORT_HDR)
    list(GET _ORT_HDR 0 _ORT_HDR0)
    get_filename_component(ORT_INCLUDE "${_ORT_HDR0}" DIRECTORY)
    get_filename_component(_ORT_ROOT "${ORT_INCLUDE}" DIRECTORY)
    set(ORT_NATIVE_DIR "${_ORT_ROOT}/lib")
else()
    set(ORT_INCLUDE    "${onnxruntime_gpu_SOURCE_DIR}/include")
    set(ORT_NATIVE_DIR "${onnxruntime_gpu_SOURCE_DIR}/lib")
endif()
set(ORT_LIB        "${ORT_NATIVE_DIR}/onnxruntime.lib")

if(NOT EXISTS "${ORT_INCLUDE}/onnxruntime_cxx_api.h")
    message(WARNING "ONNX Runtime headers not at expected path:\n  ${ORT_INCLUDE}\n"
                    "If the build fails to find onnxruntime_cxx_api.h, paste this to Claude.")
endif()

juce_add_plugin(GentSampler
    COMPANY_NAME                "JoeyTheGent"
    PLUGIN_MANUFACTURER_CODE    Jtgt
    PLUGIN_CODE                 Gsmp
    FORMATS                     VST3 Standalone
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE          # NEVER enable: JUCE's copyDir.cmake REMOVE_RECURSEs the
                                               # destination, nuking the CUDA/cuDNN DLLs deployed next to
                                               # the binary. Deploy is the custom merge-copy step below.
    VST3_AUTO_MANIFEST          FALSE          # <-- skip the build-time LoadLibrary step
    PRODUCT_NAME                "GentSampler")

juce_generate_juce_header(GentSampler)

target_sources(GentSampler PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/StemSeparator.cpp           # <-- stem engine
    Source/ModelDownloader.cpp         # <-- first-run weight downloader
    Source/Transcriber.cpp)            # <-- Basic Pitch audio-to-MIDI transcription

# Header include path for signalsmith-stretch (works whether or not it defines a CMake target)
target_include_directories(GentSampler PRIVATE ${signalsmith_stretch_SOURCE_DIR})
if(TARGET signalsmith-stretch)
    target_link_libraries(GentSampler PRIVATE signalsmith-stretch)
endif()

# ---- ONNX Runtime include + link ----
target_include_directories(GentSampler PRIVATE "${ORT_INCLUDE}")
# (no onnxruntime.lib link: the engine loads onnxruntime.dll manually at runtime,
#  so the plugin module has ZERO load-time dependency on ONNX Runtime)

target_compile_definitions(GentSampler PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

# Read source AND emit narrow string literals as UTF-8 so unicode glyphs in UI text
# (middots, arrows, etc.) decode correctly via juce::String instead of mojibaking.
target_compile_options(GentSampler PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

target_link_libraries(GentSampler
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_cryptography    # juce::SHA256 for model-download verification
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)

# ---- copy ONLY the small ONNX Runtime core DLLs next to the built VST3 binary ----
# onnxruntime.dll + the EP shim are enough for the CPU path (the CUDA provider is
# delay-loaded). The large CUDA EP DLL (onnxruntime_providers_cuda.dll) and the
# CUDA/cuDNN runtime are first-run downloads (ModelDownloader), so CPU-only installs
# stay small. (onnxruntime_providers_cuda.dll is collected into the CUDA pack instead.)
add_custom_command(TARGET GentSampler_VST3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORT_NATIVE_DIR}/onnxruntime.dll"
        "${ORT_NATIVE_DIR}/onnxruntime_providers_shared.dll"
        "$<TARGET_FILE_DIR:GentSampler_VST3>"
    VERBATIM)

# ---- same two core DLLs next to the Standalone (BACKLOG fix 2026-07-03:
# fresh-build standalones couldn't separate — copy step targeted VST3 only) ----
add_custom_command(TARGET GentSampler_Standalone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORT_NATIVE_DIR}/onnxruntime.dll"
        "${ORT_NATIVE_DIR}/onnxruntime_providers_shared.dll"
        "$<TARGET_FILE_DIR:GentSampler_Standalone>"
    VERBATIM)

# ---- deploy: merge-copy the fresh VST3 binary into the system VST3 folder ----
# NOT COPY_PLUGIN_AFTER_BUILD: JUCE's copyDir.cmake deletes the destination first
# (REMOVE_RECURSE), which wipes the CUDA/cuDNN runtime DLLs ModelDownloader parks
# next to the deployed binary, and recreating the folder under Common Files\VST3
# needs elevation. This copies ONLY the binary; it never deletes anything.
# Prereq: one-time ACL grant on the GentSampler.vst3 folder (done 2026-07-02 via
# the Desktop "Fix GentSampler VST3 Access.bat"). If FL Studio is open with
# GentSampler loaded, this step fails the build — deliberate: it means FL is
# still auditioning a stale binary. Close FL (or unload the plugin) and rebuild.
add_custom_command(TARGET GentSampler_VST3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:GentSampler_VST3>"
        "C:/Program Files/Common Files/VST3/GentSampler.vst3/Contents/x86_64-win"
    VERBATIM)

# ---- unit tests (logic-only; no JUCE link; see TEST_TARGET_TASK.md) ----
option(GENT_BUILD_TESTS "Build EngineMath unit tests" ON)
if(GENT_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/vendor/doctest.h")
    enable_testing()
    add_executable(GentSamplerTests
        tests/TestMain.cpp
        tests/SliceWindowTests.cpp
        tests/SnapTests.cpp
        tests/TriggerTests.cpp
        tests/StemMaskTests.cpp)
    target_include_directories(GentSamplerTests PRIVATE Source tests/vendor)
    target_compile_features(GentSamplerTests PRIVATE cxx_std_17)
    target_compile_options(GentSamplerTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    add_test(NAME EngineMath COMMAND GentSamplerTests)
    set_tests_properties(EngineMath PROPERTIES TIMEOUT 60)
endif()
