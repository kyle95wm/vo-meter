cmake_minimum_required(VERSION 3.22)

# =============================================================================
# Plugin identity: everything that differs between kyle95wm plugins lives in
# this block (plus the DSP/UI in Source/). scripts/new-plugin.sh fills it in.
# See CONVENTIONS.md before changing any of it, and never change PLUGIN_CODE,
# BUNDLE_ID or PRODUCT_NAME of a plugin that has been released.
# =============================================================================
set(VO_METER_PRODUCT     "VOMeter")              # CamelCase, no spaces
set(VO_METER_CODE        "VoMt")                    # unique 4-char code, >= 1 uppercase
set(VO_METER_BUNDLE_ID   "com.kyle95wm.vometer") # com.kyle95wm.<lowercase product>
set(VO_METER_GITHUB_REPO "kyle95wm/vo-meter")    # owner/repo the update checker reads
set(VO_METER_ACCENT      "0xff818cf8")              # the plugin's one accent colour (ARGB)
set(VO_METER_DESCRIPTION "Dialogue-gated LUFS meter with trim and -2.5 dBTP limiter for finishing VO")
set(VO_METER_VST3_CATEGORIES Fx)                     # e.g. Fx Dynamics / Fx EQ / Fx Tools

# The release workflow passes -DVO_METER_VERSION=<tag without the leading v>,
# so the version baked into the plugin (and used by the update checker) always
# matches the GitHub Release it ships in.
set(VO_METER_VERSION "0.2.0" CACHE STRING "Plugin version (major.minor.patch)")

# Must be set before project() to take effect. 10.13 is what both released
# plugins ship with; JUCE 8 supports it and it covers every Mac running Logic 10.4+.
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
endif()

project(VOMeter VERSION ${VO_METER_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(VO_METER_BUILD_AU "Build the Audio Unit as well as the VST3 (macOS only)" ON)
option(VO_METER_BUILD_STANDALONE "Build a standalone app (handy for quick UI testing)" OFF)
option(VO_METER_BUILD_TESTS "Build the headless test executable" ON)
option(VO_METER_BUILD_SCREENSHOT_TOOL "Build a tool that renders the UI to docs/screenshot.png" OFF)
option(VO_METER_COPY_AFTER_BUILD "Install the plugins into your user plug-in folders after building" OFF)

# ---------------------------------------------------------------------------
# JUCE: use a local checkout if one is given, otherwise fetch a pinned release.
# Keep the tag identical across all kyle95wm plugins (see CONVENTIONS.md).
# ---------------------------------------------------------------------------
set(VO_METER_JUCE_PATH "" CACHE PATH "Path to an existing JUCE checkout (optional)")

if(VO_METER_JUCE_PATH)
    add_subdirectory(${VO_METER_JUCE_PATH} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.17-security
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

set(VO_METER_FORMATS VST3)
if(APPLE AND VO_METER_BUILD_AU)
    list(APPEND VO_METER_FORMATS AU)
endif()
if(VO_METER_BUILD_STANDALONE)
    list(APPEND VO_METER_FORMATS Standalone)
endif()

juce_add_plugin(VOMeter
    PRODUCT_NAME             "${VO_METER_PRODUCT}"
    COMPANY_NAME             "kyle95wm"          # shared: groups every plugin in one Logic menu
    COMPANY_WEBSITE          "https://github.com/${VO_METER_GITHUB_REPO}"
    BUNDLE_ID                "${VO_METER_BUNDLE_ID}"
    PLUGIN_MANUFACTURER_CODE Ky95                # shared by every kyle95wm plugin
    PLUGIN_CODE              ${VO_METER_CODE}
    FORMATS                  ${VO_METER_FORMATS}
    IS_SYNTH                 FALSE
    NEEDS_CURL               TRUE                # update checker on Linux
    NEEDS_MIDI_INPUT         FALSE
    NEEDS_MIDI_OUTPUT        FALSE
    IS_MIDI_EFFECT           FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD  ${VO_METER_COPY_AFTER_BUILD}
    VST3_CATEGORIES          ${VO_METER_VST3_CATEGORIES}
    AU_MAIN_TYPE             kAudioUnitType_Effect
    AU_EXPORT_PREFIX         VOMeterAU
    DESCRIPTION              "${VO_METER_DESCRIPTION}")

set(VO_METER_PLUGIN_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/UpdateChecker.cpp)

target_sources(VOMeter PRIVATE ${VO_METER_PLUGIN_SOURCES})
target_include_directories(VOMeter PRIVATE Source)

# Read by the shared files (UpdateChecker, BrandLookAndFeel), which stay
# byte-identical across plugins.
set(VO_METER_SHARED_DEFINITIONS
    PLUGIN_GITHUB_REPO="${VO_METER_GITHUB_REPO}"
    BRAND_ACCENT_ARGB=${VO_METER_ACCENT})

target_compile_definitions(VOMeter PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    ${VO_METER_SHARED_DEFINITIONS})

target_link_libraries(VOMeter
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# The test and screenshot tools compile the plugin's processor/editor directly
# (no plugin host needed), so they need the macros the plugin wrapper would set.
set(VO_METER_TOOL_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JucePlugin_Name="${VO_METER_PRODUCT}"
    JucePlugin_VersionString="${PROJECT_VERSION}"
    JucePlugin_Build_Standalone=0
    ${VO_METER_SHARED_DEFINITIONS})

# ---------------------------------------------------------------------------
# Headless tests: ctest --test-dir build -C Release
# ---------------------------------------------------------------------------
if(VO_METER_BUILD_TESTS)
    enable_testing()
    juce_add_console_app(VOMeterTest PRODUCT_NAME "VOMeterTest")
    target_sources(VOMeterTest PRIVATE Tests/TestHost.cpp ${VO_METER_PLUGIN_SOURCES})
    target_include_directories(VOMeterTest PRIVATE Source)
    target_compile_definitions(VOMeterTest PRIVATE ${VO_METER_TOOL_DEFINITIONS})
    target_link_libraries(VOMeterTest PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_recommended_config_flags)
    add_test(NAME VOMeterTests COMMAND VOMeterTest)
endif()

# ---------------------------------------------------------------------------
# Optional: render the editor to a PNG for the README (see Tools/RenderScreenshot.cpp)
# ---------------------------------------------------------------------------
if(VO_METER_BUILD_SCREENSHOT_TOOL)
    juce_add_console_app(VOMeterScreenshot PRODUCT_NAME "VOMeterScreenshot")
    target_sources(VOMeterScreenshot PRIVATE Tools/RenderScreenshot.cpp ${VO_METER_PLUGIN_SOURCES})
    target_include_directories(VOMeterScreenshot PRIVATE Source)
    target_compile_definitions(VOMeterScreenshot PRIVATE ${VO_METER_TOOL_DEFINITIONS}
        JUCE_MODAL_LOOPS_PERMITTED=1)   # lets the tool pump the message loop while the update check finishes
    target_link_libraries(VOMeterScreenshot PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags)
endif()
