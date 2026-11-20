cmake_minimum_required(VERSION 3.22)

project(AltraPlugHost VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(external/JUCE)

# Built plugins are copied here, so Live's "VST3 Custom Folder" can point at one place.
set(ALTRA_VST3_DIR "${CMAKE_BINARY_DIR}/VST3" CACHE PATH "Where built VST3 bundles are copied")

# Two variants of the same code: Live puts instruments and effects in different slots, and a
# VST3 declares which one it is up front.
function(altra_add_variant target product_name plugin_code is_synth)
    juce_add_plugin(${target}
        COMPANY_NAME             "Altra"
        PLUGIN_MANUFACTURER_CODE Altr
        PLUGIN_CODE              ${plugin_code}
        FORMATS                  VST3
        PRODUCT_NAME             "${product_name}"
        IS_SYNTH                 ${is_synth}
        NEEDS_MIDI_INPUT         TRUE
        NEEDS_MIDI_OUTPUT        FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD  TRUE
        VST3_COPY_DIR            "${ALTRA_VST3_DIR}"
        NEEDS_WEB_BROWSER        FALSE
        NEEDS_CURL               FALSE)

    target_sources(${target} PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/HostedEditorWindow.cpp
        Source/NativeWindowHelpers.cpp)

    target_compile_definitions(${target} PUBLIC
        ALTRA_IS_INSTRUMENT=$<BOOL:${is_synth}>
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_VST=0
        JUCE_PLUGINHOST_AU=0
        JUCE_PLUGINHOST_LV2=0
        JUCE_PLUGINHOST_LADSPA=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

    target_link_libraries(${target}
        PRIVATE
            juce::juce_audio_utils
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endfunction()

altra_add_variant(AltraPlugHost           "AltraPlugHost"            Aphf FALSE)
altra_add_variant(AltraPlugHostInstrument "AltraPlugHost Instrument" Aphi TRUE)
