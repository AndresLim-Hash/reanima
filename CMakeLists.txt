cmake_minimum_required(VERSION 3.22)

project(REANIMA VERSION 0.9.5)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# macOS: one binary for Apple Silicon and Intel, from macOS 11 on, with an Audio Unit as well.
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
    set(REANIMA_FORMATS VST3 AU Standalone)
else()
    set(REANIMA_FORMATS VST3 Standalone)
endif()

add_subdirectory(external/JUCE)

# Pitch and time apart (Warp): Signalsmith Stretch and Signalsmith Linear, MIT, header-only.
add_library(signalsmith_stretch INTERFACE)
target_include_directories(signalsmith_stretch SYSTEM INTERFACE
    external/signalsmith-stretch-1.4.0
    external/signalsmith-linear-0.6.4/include)

# LOOP Complex: Bungee (Parabola Research, MPL-2.0: its files stay under MPL and unmodified; Eigen MPL-2.0, PFFFT
# BSD-like), measured best of Signalsmith, Rubber Band R2/R3 and Bungee on 8 kinds of loop (research/stretch_2026-11-17).
set(BUNGEE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/bungee-61f52bf)
file(GLOB BUNGEE_SOURCES ${BUNGEE_DIR}/src/*.cpp)
add_library(bungee_basic STATIC ${BUNGEE_SOURCES} ${BUNGEE_DIR}/submodules/pffft/pffft.c ${BUNGEE_DIR}/submodules/pffft/fftpack.c)
set_target_properties(bungee_basic PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_include_directories(bungee_basic PRIVATE ${BUNGEE_DIR}/submodules/eigen ${BUNGEE_DIR}/submodules/pffft ${BUNGEE_DIR})
target_compile_definitions(bungee_basic PRIVATE BUNGEE_VISIBILITY= BUNGEE_SELF_TEST=0 eigen_assert=BUNGEE_ASSERT1 EIGEN_DONT_PARALLELIZE=1 _USE_MATH_DEFINES
    BUNGEE_VERSION="61f52bf")
target_compile_options(bungee_basic PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc /W0> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fwrapv -w>)
target_include_directories(signalsmith_stretch SYSTEM INTERFACE ${BUNGEE_DIR})
target_compile_definitions(signalsmith_stretch INTERFACE BUNGEE_VISIBILITY=)
target_link_libraries(signalsmith_stretch INTERFACE bungee_basic)

# LOOP "Rubber Band": Rubber Band Library 4.0.0 (Breakfast Quay, GPL v2 or later; R3 engine). With it Reanima as a whole
# is distributed under the AGPLv3 (JUCE's open licence), see LICENSE and LICENCIAS_TERCEROS.md.
add_library(rubberband_lib STATIC ${CMAKE_CURRENT_SOURCE_DIR}/external/rubberband-4.0.0/single/RubberBandSingle.cpp)
set_target_properties(rubberband_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(rubberband_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/external/rubberband-4.0.0)
target_compile_definitions(rubberband_lib PRIVATE _USE_MATH_DEFINES NOMINMAX)
target_compile_options(rubberband_lib PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj /EHsc /W0> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>)
target_link_libraries(signalsmith_stretch INTERFACE rubberband_lib)

# Recordings of instrument mechanisms (CC0, see Resources/Mechanism/CREDITOS.txt), kept inside the plugin.
file(GLOB REANIMA_MECHANISM_FILES CONFIGURE_DEPENDS Resources/Mechanism/*.flac)
juce_add_binary_data(ReanimaMechanism
    HEADER_NAME MechanismData.h
    NAMESPACE MechanismData
    SOURCES ${REANIMA_MECHANISM_FILES} Resources/Mechanism/index.csv
            Resources/Demo/demo_pluck.wav)   # the sample a new, empty instance starts with

juce_add_plugin(Reanima
    COMPANY_NAME "Blackpanda"
    COMPANY_WEBSITE ""
    PLUGIN_MANUFACTURER_CODE Bpnd
    PLUGIN_CODE Rnm1
    FORMATS ${REANIMA_FORMATS}
    BUNDLE_ID com.blackpanda.reanima
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    PRODUCT_NAME "Reanima"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Instrument Sampler
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(Reanima
    PRIVATE
        Source/Params.cpp
        Source/PluginProcessor.cpp
        Source/FactoryPresets.cpp
        Source/Randomizer.cpp
        Source/PluginEditor.cpp
        Source/SampleBrowser.cpp
        Source/Displays.cpp
        Source/EditPage.cpp
        Source/Guide.cpp
        Source/TabGuides.cpp
        Source/FxPage.cpp
        Source/FxRack.cpp
        Source/HelpText.cpp
        Source/Reconstruct.cpp
        Source/SampleAnalysis.cpp
        Source/KeyDetect.cpp
        Source/StepSeq.cpp
        Source/SynthEngine.cpp
        Source/Mechanism.cpp
        Source/DroneStretch.cpp
        Source/WaveformView.cpp)

target_include_directories(Reanima PRIVATE Source)

# Source files contain Spanish UI text in UTF-8.
target_compile_options(Reanima PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

target_compile_definitions(Reanima
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Reanima
    PRIVATE
        ReanimaMechanism
        signalsmith_stretch
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Offline checks of the analysis and engine (no GUI, no DAW needed).
juce_add_console_app(ReanimaTests PRODUCT_NAME "ReanimaTests")

target_sources(ReanimaTests
    PRIVATE
        tests/EngineTests.cpp
        Source/Params.cpp
        Source/FxRack.cpp
        Source/Reconstruct.cpp
        Source/SampleAnalysis.cpp
        Source/KeyDetect.cpp
        Source/StepSeq.cpp
        Source/SynthEngine.cpp
        Source/Mechanism.cpp
        Source/DroneStretch.cpp)

target_include_directories(ReanimaTests PRIVATE Source)
target_compile_options(ReanimaTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_definitions(ReanimaTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)

target_link_libraries(ReanimaTests
    PRIVATE
        ReanimaMechanism
        signalsmith_stretch
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Renders the editor offscreen to PNG files (UI check without a DAW).
juce_add_console_app(ReanimaSnapshot PRODUCT_NAME "ReanimaSnapshot")

target_sources(ReanimaSnapshot
    PRIVATE
        tests/UiSnapshot.cpp
        Source/Params.cpp
        Source/PluginProcessor.cpp
        Source/FactoryPresets.cpp
        Source/Randomizer.cpp
        Source/PluginEditor.cpp
        Source/SampleBrowser.cpp
        Source/Displays.cpp
        Source/EditPage.cpp
        Source/Guide.cpp
        Source/TabGuides.cpp
        Source/FxPage.cpp
        Source/FxRack.cpp
        Source/HelpText.cpp
        Source/Reconstruct.cpp
        Source/SampleAnalysis.cpp
        Source/KeyDetect.cpp
        Source/StepSeq.cpp
        Source/SynthEngine.cpp
        Source/Mechanism.cpp
        Source/DroneStretch.cpp
        Source/WaveformView.cpp)

target_include_directories(ReanimaSnapshot PRIVATE Source)
target_compile_options(ReanimaSnapshot PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_definitions(ReanimaSnapshot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JucePlugin_Name="Reanima" JucePlugin_VersionString="${PROJECT_VERSION}" JUCE_MODAL_LOOPS_PERMITTED=1)

target_link_libraries(ReanimaSnapshot
    PRIVATE
        ReanimaMechanism
        signalsmith_stretch
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Counts allocations and frees on the audio thread while a simulated host plays every engine and effect.
juce_add_console_app(ReanimaRtCheck PRODUCT_NAME "ReanimaRtCheck")

target_sources(ReanimaRtCheck
    PRIVATE
        tests/RtCheck.cpp
        Source/Params.cpp
        Source/PluginProcessor.cpp
        Source/FactoryPresets.cpp
        Source/Randomizer.cpp
        Source/PluginEditor.cpp
        Source/SampleBrowser.cpp
        Source/Displays.cpp
        Source/EditPage.cpp
        Source/Guide.cpp
        Source/TabGuides.cpp
        Source/FxPage.cpp
        Source/FxRack.cpp
        Source/HelpText.cpp
        Source/Reconstruct.cpp
        Source/SampleAnalysis.cpp
        Source/KeyDetect.cpp
        Source/StepSeq.cpp
        Source/SynthEngine.cpp
        Source/Mechanism.cpp
        Source/DroneStretch.cpp
        Source/WaveformView.cpp)

target_include_directories(ReanimaRtCheck PRIVATE Source)
target_compile_options(ReanimaRtCheck PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_definitions(ReanimaRtCheck PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JucePlugin_Name="Reanima" JucePlugin_VersionString="${PROJECT_VERSION}" JUCE_MODAL_LOOPS_PERMITTED=1)

target_link_libraries(ReanimaRtCheck
    PRIVATE
        ReanimaMechanism
        signalsmith_stretch
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Audio-thread CPU per block in playing patterns (fresh instance, real sample).
juce_add_console_app(ReanimaCpuBench PRODUCT_NAME "ReanimaCpuBench")

target_sources(ReanimaCpuBench
    PRIVATE
        tests/CpuBench.cpp
        Source/Params.cpp
        Source/PluginProcessor.cpp
        Source/FactoryPresets.cpp
        Source/Randomizer.cpp
        Source/PluginEditor.cpp
        Source/SampleBrowser.cpp
        Source/Displays.cpp
        Source/EditPage.cpp
        Source/Guide.cpp
        Source/TabGuides.cpp
        Source/FxPage.cpp
        Source/FxRack.cpp
        Source/HelpText.cpp
        Source/Reconstruct.cpp
        Source/SampleAnalysis.cpp
        Source/KeyDetect.cpp
        Source/StepSeq.cpp
        Source/SynthEngine.cpp
        Source/Mechanism.cpp
        Source/DroneStretch.cpp
        Source/WaveformView.cpp)

target_include_directories(ReanimaCpuBench PRIVATE Source)
target_compile_options(ReanimaCpuBench PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_definitions(ReanimaCpuBench PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JucePlugin_Name="Reanima" JucePlugin_VersionString="${PROJECT_VERSION}" JUCE_MODAL_LOOPS_PERMITTED=1)

target_link_libraries(ReanimaCpuBench
    PRIVATE
        ReanimaMechanism
        signalsmith_stretch
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Every factory preset rendered on several samples, for the preset audit.
juce_add_console_app(ReanimaPresetRender PRODUCT_NAME "ReanimaPresetRender")

target_sources(ReanimaPresetRender
    PRIVATE
        tests/PresetRender.cpp
        Source/Params.cpp
        Source/PluginProcessor.cpp
        Source/FactoryPresets.cpp
        Source/Randomizer.cpp
        Source/PluginEditor.cpp
        Source/SampleBrowser.cpp
        Source/Displays.cpp
        Source/EditPage.cpp
        Source/Guide.cpp
        Source/TabGuides.cpp
        Source/FxPage.cpp
        Source/FxRack.cpp
        Source/HelpText.cpp
        Source/Reconstruct.cpp
        Source/SampleAnalysis.cpp
        Source/KeyDetect.cpp
        Source/StepSeq.cpp
        Source/SynthEngine.cpp
        Source/Mechanism.cpp
        Source/DroneStretch.cpp
        Source/WaveformView.cpp)

target_include_directories(ReanimaPresetRender PRIVATE Source)
target_compile_options(ReanimaPresetRender PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_definitions(ReanimaPresetRender PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JucePlugin_Name="Reanima" JucePlugin_VersionString="${PROJECT_VERSION}" JUCE_MODAL_LOOPS_PERMITTED=1)

target_link_libraries(ReanimaPresetRender
    PRIVATE
        ReanimaMechanism
        signalsmith_stretch
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Clicks and crackles per preset at host blocks of 64, 512 and 37 samples.
juce_add_console_app(ReanimaClickCheck PRODUCT_NAME "ReanimaClickCheck")

target_sources(ReanimaClickCheck
    PRIVATE
        tests/ClickCheck.cpp
        Source/Params.cpp
        Source/PluginProcessor.cpp
        Source/FactoryPresets.cpp
        Source/Randomizer.cpp
        Source/PluginEditor.cpp
        Source/SampleBrowser.cpp
        Source/Displays.cpp
        Source/EditPage.cpp
        Source/Guide.cpp
        Source/TabGuides.cpp
        Source/FxPage.cpp
        Source/FxRack.cpp
        Source/HelpText.cpp
        Source/Reconstruct.cpp
        Source/SampleAnalysis.cpp
        Source/KeyDetect.cpp
        Source/StepSeq.cpp
        Source/SynthEngine.cpp
        Source/Mechanism.cpp
        Source/DroneStretch.cpp
        Source/WaveformView.cpp)

target_include_directories(ReanimaClickCheck PRIVATE Source)
target_compile_options(ReanimaClickCheck PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_definitions(ReanimaClickCheck PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JucePlugin_Name="Reanima" JucePlugin_VersionString="${PROJECT_VERSION}" JUCE_MODAL_LOOPS_PERMITTED=1)

target_link_libraries(ReanimaClickCheck
    PRIVATE
        ReanimaMechanism
        signalsmith_stretch
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Listening demos of engine D, with the numbers that matter for each one.
juce_add_console_app(ReanimaDemos PRODUCT_NAME "ReanimaDemos")

target_sources(ReanimaDemos
    PRIVATE
        tests/Demos.cpp
        Source/Params.cpp
        Source/FxRack.cpp
        Source/Reconstruct.cpp
        Source/SampleAnalysis.cpp
        Source/KeyDetect.cpp
        Source/StepSeq.cpp
        Source/SynthEngine.cpp
        Source/Mechanism.cpp
        Source/DroneStretch.cpp)

target_include_directories(ReanimaDemos PRIVATE Source)
target_compile_options(ReanimaDemos PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_definitions(ReanimaDemos PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)

target_link_libraries(ReanimaDemos
    PRIVATE
        ReanimaMechanism
        signalsmith_stretch
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Runs engine D over a whole folder of samples and reports each one in a CSV.
juce_add_console_app(ReanimaBatch PRODUCT_NAME "ReanimaBatch")

target_sources(ReanimaBatch
    PRIVATE
        tests/Batch.cpp
        Source/Params.cpp
        Source/FxRack.cpp
        Source/Reconstruct.cpp
        Source/SampleAnalysis.cpp
        Source/KeyDetect.cpp
        Source/StepSeq.cpp
        Source/SynthEngine.cpp
        Source/Mechanism.cpp
        Source/DroneStretch.cpp)

target_include_directories(ReanimaBatch PRIVATE Source)
target_compile_options(ReanimaBatch PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_definitions(ReanimaBatch PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)

target_link_libraries(ReanimaBatch
    PRIVATE
        ReanimaMechanism
        signalsmith_stretch
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Measures what happens after the note-off, over a folder of samples.
juce_add_console_app(ReanimaRelease PRODUCT_NAME "ReanimaRelease")

target_sources(ReanimaRelease
    PRIVATE
        tests/ReleaseCheck.cpp
        Source/Params.cpp
        Source/FxRack.cpp
        Source/Reconstruct.cpp
        Source/SampleAnalysis.cpp
        Source/KeyDetect.cpp
        Source/StepSeq.cpp
        Source/SynthEngine.cpp
        Source/Mechanism.cpp
        Source/DroneStretch.cpp)

target_include_directories(ReanimaRelease PRIVATE Source)
target_compile_options(ReanimaRelease PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_definitions(ReanimaRelease PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)

target_link_libraries(ReanimaRelease
    PRIVATE
        ReanimaMechanism
        signalsmith_stretch
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Measures every promise of the plugin at each supported sample rate.
juce_add_console_app(ReanimaAudit PRODUCT_NAME "ReanimaAudit")

target_sources(ReanimaAudit
    PRIVATE
        tests/Audit.cpp
        Source/Params.cpp
        Source/PluginProcessor.cpp
        Source/FactoryPresets.cpp
        Source/Randomizer.cpp
        Source/PluginEditor.cpp
        Source/SampleBrowser.cpp
        Source/Displays.cpp
        Source/EditPage.cpp
        Source/Guide.cpp
        Source/TabGuides.cpp
        Source/FxPage.cpp
        Source/FxRack.cpp
        Source/HelpText.cpp
        Source/Reconstruct.cpp
        Source/SampleAnalysis.cpp
        Source/KeyDetect.cpp
        Source/StepSeq.cpp
        Source/SynthEngine.cpp
        Source/Mechanism.cpp
        Source/DroneStretch.cpp
        Source/WaveformView.cpp)

target_include_directories(ReanimaAudit PRIVATE Source)
target_compile_options(ReanimaAudit PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_definitions(ReanimaAudit PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JucePlugin_Name="Reanima" JucePlugin_VersionString="${PROJECT_VERSION}" JUCE_MODAL_LOOPS_PERMITTED=1)

target_link_libraries(ReanimaAudit
    PRIVATE
        ReanimaMechanism
        signalsmith_stretch
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# The MASTER output stage lab: the old and new stage over the same signals (24-11-2026).
juce_add_console_app(ReanimaMasterLab PRODUCT_NAME "ReanimaMasterLab")

target_sources(ReanimaMasterLab
    PRIVATE
        tests/MasterLab.cpp
        Source/Params.cpp
        Source/PluginProcessor.cpp
        Source/FactoryPresets.cpp
        Source/Randomizer.cpp
        Source/PluginEditor.cpp
        Source/SampleBrowser.cpp
        Source/Displays.cpp
        Source/EditPage.cpp
        Source/Guide.cpp
        Source/TabGuides.cpp
        Source/FxPage.cpp
        Source/FxRack.cpp
        Source/HelpText.cpp
        Source/Reconstruct.cpp
        Source/SampleAnalysis.cpp
        Source/KeyDetect.cpp
        Source/StepSeq.cpp
        Source/SynthEngine.cpp
        Source/Mechanism.cpp
        Source/DroneStretch.cpp
        Source/WaveformView.cpp)

target_include_directories(ReanimaMasterLab PRIVATE Source)
target_compile_options(ReanimaMasterLab PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_definitions(ReanimaMasterLab PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JucePlugin_Name="Reanima" JucePlugin_VersionString="${PROJECT_VERSION}" JUCE_MODAL_LOOPS_PERMITTED=1)

target_link_libraries(ReanimaMasterLab
    PRIVATE
        ReanimaMechanism
        signalsmith_stretch
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Loads the built .vst3 through the VST3 interface, as a host does.
juce_add_console_app(ReanimaHostCheck PRODUCT_NAME "ReanimaHostCheck")

target_sources(ReanimaHostCheck PRIVATE tests/HostCheck.cpp)
target_compile_options(ReanimaHostCheck PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_definitions(ReanimaHostCheck PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1)

target_link_libraries(ReanimaHostCheck
    PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Time-stretch lab (evaluation only): Signalsmith, Rubber Band (GPL, never linked into the plug-in), hybrid.
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/eval/rubberband/single/RubberBandSingle.cpp)
    juce_add_console_app(ReanimaStretchLab PRODUCT_NAME "ReanimaStretchLab")
    target_sources(ReanimaStretchLab PRIVATE tests/StretchLab.cpp)
    target_include_directories(ReanimaStretchLab PRIVATE external/signalsmith-stretch-1.4.0)
    target_compile_definitions(ReanimaStretchLab PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 _USE_MATH_DEFINES NOMINMAX)
    target_compile_options(ReanimaStretchLab PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /bigobj>)
    target_link_libraries(ReanimaStretchLab PRIVATE signalsmith_stretch juce::juce_audio_formats juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags)
endif()

# Resizing the window as a user and a host do it, with audio playing: smooth, controls alive, nothing sounding changed.
if(WIN32)
juce_add_console_app(ReanimaResizeCheck PRODUCT_NAME "ReanimaResizeCheck")
target_sources(ReanimaResizeCheck
    PRIVATE
        tests/ResizeCheck.cpp
        Source/Params.cpp
        Source/PluginProcessor.cpp
        Source/FactoryPresets.cpp
        Source/Randomizer.cpp
        Source/PluginEditor.cpp
        Source/SampleBrowser.cpp
        Source/Displays.cpp
        Source/EditPage.cpp
        Source/Guide.cpp
        Source/TabGuides.cpp
        Source/FxPage.cpp
        Source/FxRack.cpp
        Source/HelpText.cpp
        Source/Reconstruct.cpp
        Source/SampleAnalysis.cpp
        Source/KeyDetect.cpp
        Source/StepSeq.cpp
        Source/SynthEngine.cpp
        Source/Mechanism.cpp
        Source/DroneStretch.cpp
        Source/WaveformView.cpp)
target_include_directories(ReanimaResizeCheck PRIVATE Source)
target_compile_options(ReanimaResizeCheck PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_definitions(ReanimaResizeCheck PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JucePlugin_Name="Reanima" JucePlugin_VersionString="${PROJECT_VERSION}" JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(ReanimaResizeCheck
    PRIVATE
        ReanimaMechanism
        signalsmith_stretch
        juce::juce_audio_utils
        juce::juce_dsp
        psapi
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endif()
