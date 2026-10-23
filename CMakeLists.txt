cmake_minimum_required(VERSION 3.22)

# CI passes its run number so every build has its own version: 0.2.<build>.
set(TAPEFOUR_BUILD_NUMBER "0" CACHE STRING "Build number (patch version)")
project(TapeFour VERSION 0.2.${TAPEFOUR_BUILD_NUMBER} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# A standalone .exe: link the MSVC runtime statically so nothing extra needs installing.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# JUCE 8.0.11 bundles the (now GPLv3) ASIO headers and the MIT-licensed VST3 SDK.
set(TAPEFOUR_JUCE_DIR "" CACHE PATH "Use a local JUCE checkout instead of downloading it")
include(FetchContent)

if(TAPEFOUR_JUCE_DIR)
    add_subdirectory(${TAPEFOUR_JUCE_DIR} JUCE)
else()
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.11
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# CLAP's headers (MIT licence, header-only): TAPE·FOUR hosts CLAP plug-ins itself.
FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG 1.2.7
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(clap)
set(TAPEFOUR_CLAP_INCLUDE ${clap_SOURCE_DIR}/include)

# Sanitizer builds for CI: -DTAPEFOUR_SANITIZE=address or thread
set(TAPEFOUR_SANITIZE "" CACHE STRING "address, thread, or empty")
if(TAPEFOUR_SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(TAPEFOUR_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

# Every audio system each platform has: ASIO, WASAPI and DirectSound on Windows;
# CoreAudio on macOS; ALSA and JACK on Linux.
# Plug-ins: VST3 and LV2 everywhere, Audio Units on macOS, LADSPA on Linux. CLAP is hosted by
# TAPE·FOUR itself (Source/ClapFormat.cpp), since JUCE doesn't host it.
set(TAPEFOUR_PLATFORM_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1)
if(WIN32)
    list(APPEND TAPEFOUR_PLATFORM_DEFS JUCE_ASIO=1 JUCE_WASAPI=1 JUCE_DIRECTSOUND=1 JUCE_PLUGINHOST_AU=0 JUCE_PLUGINHOST_LADSPA=0)
elseif(APPLE)
    list(APPEND TAPEFOUR_PLATFORM_DEFS JUCE_PLUGINHOST_AU=1 JUCE_PLUGINHOST_LADSPA=0)
else()
    list(APPEND TAPEFOUR_PLATFORM_DEFS JUCE_ALSA=1 JUCE_JACK=1 JUCE_PLUGINHOST_AU=0 JUCE_PLUGINHOST_LADSPA=1)
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
    BUNDLE_ID "com.78hz.tapefour"
    VERSION ${PROJECT_VERSION}
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "TAPE-FOUR records from your audio inputs.")

# The engine, shared by the app and the test programs.
set(TAPEFOUR_ENGINE_SOURCES
    Source/Tape.cpp
    Source/TapeEngine.cpp
    Source/TapeWriter.cpp
    Source/Blocks.cpp
    Source/MidiHub.cpp
    Source/MidiMapper.cpp
    Source/ChannelStrip.cpp
    Source/AudioEngine.cpp
    Source/DeckState.cpp
    Source/TapeShelf.cpp
    Source/PluginScan.cpp
    Source/ClapFormat.cpp)

target_sources(TapeFour PRIVATE
    Source/Main.cpp
    Source/CrashHandler.cpp
    ${TAPEFOUR_ENGINE_SOURCES}
    Source/DeckLookAndFeel.cpp
    Source/CassetteComponent.cpp
    Source/StripComponent.cpp
    Source/DeckComponent.cpp
    Source/ChainEditor.cpp
    Source/InlayCard.cpp
    Source/SetupWindow.cpp)

target_compile_definitions(TapeFour PRIVATE
    ${TAPEFOUR_PLATFORM_DEFS}
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:TapeFour,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:TapeFour,JUCE_VERSION>")

target_compile_options(TapeFour PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_include_directories(TapeFour PRIVATE ${TAPEFOUR_CLAP_INCLUDE})

target_link_libraries(TapeFour
    PRIVATE
        TapeFourFonts
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
        $<$<PLATFORM_ID:Windows>:dbghelp>
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Headless checks: cmake -DTAPEFOUR_TESTS=ON, then run EngineTest, MixerTest and ScanTest.
option(TAPEFOUR_TESTS "Build the test programs" OFF)
if(TAPEFOUR_TESTS)
    function(tapefour_test name)
        juce_add_console_app(${name} PRODUCT_NAME "${name}")
        target_sources(${name} PRIVATE ${ARGN})
        target_compile_definitions(${name} PRIVATE ${TAPEFOUR_PLATFORM_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
        target_compile_options(${name} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
        target_include_directories(${name} PRIVATE ${TAPEFOUR_CLAP_INCLUDE})
        target_link_libraries(${name} PRIVATE juce::juce_audio_utils juce::juce_dsp
            PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    endfunction()

    tapefour_test(EngineTest Tests/EngineTest.cpp Source/Tape.cpp Source/TapeEngine.cpp)
    tapefour_test(MixerTest Tests/MixerTest.cpp ${TAPEFOUR_ENGINE_SOURCES})
    tapefour_test(ScanTest Tests/ScanTest.cpp Source/PluginScan.cpp Source/ClapFormat.cpp)

    # A "VST3" that crashes as soon as it is loaded, laid out as each platform's bundle,
    # for ScanTest: TapeFour must survive scanning it.
    add_library(CrashPlugin MODULE Tests/fixtures/CrashPlugin.c)
    set(crashBundle ${CMAKE_BINARY_DIR}/crashplugin/Crash.vst3)
    if(WIN32)
        set_target_properties(CrashPlugin PROPERTIES OUTPUT_NAME "Crash" SUFFIX ".vst3"
            LIBRARY_OUTPUT_DIRECTORY ${crashBundle}/Contents/x86_64-win
            LIBRARY_OUTPUT_DIRECTORY_RELEASE ${crashBundle}/Contents/x86_64-win
            LIBRARY_OUTPUT_DIRECTORY_DEBUG ${crashBundle}/Contents/x86_64-win)
    elseif(APPLE)
        set_target_properties(CrashPlugin PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION "vst3" OUTPUT_NAME "Crash"
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/crashplugin
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.78hz.crashplugin"
            MACOSX_BUNDLE_BUNDLE_NAME "Crash")
    else()
        set_target_properties(CrashPlugin PROPERTIES OUTPUT_NAME "Crash" PREFIX "" SUFFIX ".so"
            LIBRARY_OUTPUT_DIRECTORY ${crashBundle}/Contents/${CMAKE_SYSTEM_PROCESSOR}-linux)
    endif()
    add_dependencies(ScanTest CrashPlugin)

    # Two tiny CLAP plug-ins for MixerTest: build/testclap/TestClap.clap
    add_library(TestClap MODULE Tests/fixtures/TestClap.c)
    target_include_directories(TestClap PRIVATE ${TAPEFOUR_CLAP_INCLUDE})
    if(APPLE)
        set_target_properties(TestClap PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION "clap" OUTPUT_NAME "TestClap"
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/testclap
            LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/testclap
            LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/testclap
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.78hz.testclap"
            MACOSX_BUNDLE_BUNDLE_NAME "TestClap")
    else()
        set_target_properties(TestClap PROPERTIES OUTPUT_NAME "TestClap" PREFIX "" SUFFIX ".clap"
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/testclap
            LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/testclap
            LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/testclap)
    endif()
    if(NOT WIN32)
        target_link_libraries(TestClap PRIVATE m)
    endif()
    add_dependencies(MixerTest TestClap)
    target_compile_definitions(MixerTest PRIVATE TAPEFOUR_TEST_CLAP="${CMAKE_BINARY_DIR}/testclap/TestClap.clap")
endif()
