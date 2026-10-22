cmake_minimum_required(VERSION 3.22)

project(TapeFour VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# A standalone .exe: link the MSVC runtime statically so nothing extra needs installing.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# JUCE 8.0.11 bundles the (now GPLv3) ASIO headers and the MIT-licensed VST3 SDK.
set(TAPEFOUR_JUCE_DIR "" CACHE PATH "Use a local JUCE checkout instead of downloading it")

if(TAPEFOUR_JUCE_DIR)
    add_subdirectory(${TAPEFOUR_JUCE_DIR} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.11
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_binary_data(TapeFourFonts SOURCES
    Resources/fonts/BarlowCondensed-SemiBold.ttf
    Resources/fonts/BarlowCondensed-Bold.ttf
    Resources/fonts/BarlowCondensed-ExtraBoldItalic.ttf
    Resources/fonts/ShareTechMono-Regular.ttf
    Resources/fonts/Caveat-Variable.ttf)

juce_add_gui_app(TapeFour
    PRODUCT_NAME "TapeFour"
    COMPANY_NAME "78Hz"
    VERSION 0.1.0)

target_sources(TapeFour PRIVATE
    Source/Main.cpp
    Source/Tape.cpp
    Source/TapeEngine.cpp
    Source/Instruments.cpp
    Source/AudioEngine.cpp
    Source/TapeShelf.cpp
    Source/DeckLookAndFeel.cpp
    Source/CassetteComponent.cpp
    Source/StripComponent.cpp
    Source/DeckComponent.cpp
    Source/SetupWindow.cpp)

target_compile_definitions(TapeFour PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:TapeFour,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:TapeFour,JUCE_VERSION>"
    JUCE_ASIO=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_PLUGINHOST_LV2=0)

target_compile_options(TapeFour PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

target_link_libraries(TapeFour
    PRIVATE
        TapeFourFonts
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Headless engine checks: cmake -DTAPEFOUR_TESTS=ON, then run EngineTest
option(TAPEFOUR_TESTS "Build the tape engine test program" OFF)
if(TAPEFOUR_TESTS)
    juce_add_console_app(EngineTest PRODUCT_NAME "EngineTest")
    target_sources(EngineTest PRIVATE Tests/EngineTest.cpp Source/Tape.cpp Source/TapeEngine.cpp)
    target_compile_definitions(EngineTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(EngineTest PRIVATE juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()

if(TAPEFOUR_TESTS)
    juce_add_console_app(MixerTest PRODUCT_NAME "MixerTest")
    target_sources(MixerTest PRIVATE Tests/MixerTest.cpp Source/Tape.cpp Source/TapeEngine.cpp
                                     Source/Instruments.cpp Source/AudioEngine.cpp)
    target_compile_definitions(MixerTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1)
    target_link_libraries(MixerTest PRIVATE juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
