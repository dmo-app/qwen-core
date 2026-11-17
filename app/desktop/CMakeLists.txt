# Core desktop app: the JUCE window. The engine runs as its own process (core-engine.exe, next to Core.exe).
# JUCE comes from the root CMakeLists (pinned there).

juce_add_gui_app(CoreApp
  PRODUCT_NAME "Core"
  COMPANY_NAME "Core"
  VERSION ${CORE_VERSION}
  ICON_BIG ${CMAKE_CURRENT_SOURCE_DIR}/resources/core-icon.png
  ICON_SMALL ${CMAKE_CURRENT_SOURCE_DIR}/resources/core-icon-small.png  # 16/32 px: wider gaps, so the C stays segmented
  NEEDS_WEB_BROWSER FALSE
  NEEDS_CURL FALSE)
if(WIN32)
  # The icon is made at configure time, but Ninja does not see it as an input of the .rc: rebuild the .rc when the PNGs change.
  get_target_property(core_juce_code CoreApp JUCE_GENERATED_SOURCES_DIRECTORY)
  set_source_files_properties(${core_juce_code}/CoreApp_resources.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/core-icon.png;${CMAKE_CURRENT_SOURCE_DIR}/resources/core-icon-small.png;${core_juce_code}/icon.ico")
endif()
target_sources(CoreApp PRIVATE src/Main.cpp src/MainComponent.cpp src/EngineSession.cpp src/PatternEditor.cpp src/Waveforms.cpp src/Stretch.cpp src/Plugins.cpp src/Autosave.cpp src/Diagnostics.cpp src/Theme.cpp src/LibraryPanel.cpp src/InspectorPanel.cpp src/Settings.cpp src/Keymap.cpp src/Mixer.cpp src/SCorePanel.cpp src/ChoiceDialog.cpp src/BuildupPanel.cpp src/PatternsWindow.cpp src/ToolDock.cpp src/ProposalsPanel.cpp src/Language.cpp)
target_sources(CoreApp PRIVATE src/GlobalKeys.cpp)  # Space plays / stops in every window
target_compile_definitions(CoreApp PRIVATE
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:CoreApp,JUCE_PRODUCT_NAME>"
  JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:CoreApp,JUCE_VERSION>")
target_link_libraries(CoreApp
  PRIVATE juce::juce_gui_basics juce::juce_audio_devices juce::juce_audio_formats core_engine_client core_domain core_previews core_version core_control core_sampling
          $<$<PLATFORM_ID:Windows>:dbghelp>
  PUBLIC juce::juce_recommended_config_flags)

add_dependencies(CoreApp core_version_header)

# The self-test plays its own test plugins (app/selftest-plugins): Core's sound comes from plugins.
if(TARGET CoreTestSynth_VST3)
  add_dependencies(CoreApp CoreTestSynth_VST3 CoreTestGain_VST3)
  target_compile_definitions(CoreApp PRIVATE
    CORE_TEST_SYNTH_VST3="$<GENEX_EVAL:$<TARGET_PROPERTY:CoreTestSynth_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>"
    CORE_TEST_GAIN_VST3="$<GENEX_EVAL:$<TARGET_PROPERTY:CoreTestGain_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")
endif()

# A runnable folder: Core.exe, core-engine.exe, the demo and the style templates side by side. The demo is the psy demo
# without its kick roll (8 kicks in one beat sound like clicks at fast tempos).
add_dependencies(CoreApp core-engine)
add_custom_command(TARGET CoreApp POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:core-engine> $<TARGET_FILE_DIR:CoreApp>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/demo.core-document.json
          $<TARGET_FILE_DIR:CoreApp>/demo.core-document.json)

# Resources that ship beside the app (audit 29): the copies used to live in POST_BUILD, so they only refreshed
# when the .exe relinked — a templates/lang/patterns/pcore edit alone left a stale app folder, package and zip.
# Now one stamped step depends on the files themselves (CONFIGURE_DEPENDS re-globs on any add/remove), and both
# CoreApp and core-package depend on it. The folders are removed before copying, so a file deleted from the repo
# leaves the app too (the shipped templates are exactly the repo's — also lang, patterns and pcore now).
file(GLOB_RECURSE CORE_APP_RESOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/../../templates/*
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lang/*
  ${CMAKE_CURRENT_SOURCE_DIR}/../../patterns/*
  ${CMAKE_CURRENT_SOURCE_DIR}/../../pcore/*)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/core_resources.stamp
  COMMAND ${CMAKE_COMMAND} -E remove_directory $<TARGET_FILE_DIR:CoreApp>/templates
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../../templates $<TARGET_FILE_DIR:CoreApp>/templates
  COMMAND ${CMAKE_COMMAND} -E remove_directory $<TARGET_FILE_DIR:CoreApp>/lang
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../../lang $<TARGET_FILE_DIR:CoreApp>/lang
  COMMAND ${CMAKE_COMMAND} -E remove_directory $<TARGET_FILE_DIR:CoreApp>/patterns
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../../patterns $<TARGET_FILE_DIR:CoreApp>/patterns
  COMMAND ${CMAKE_COMMAND} -E remove_directory $<TARGET_FILE_DIR:CoreApp>/pcore
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../../pcore $<TARGET_FILE_DIR:CoreApp>/pcore
  COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/core_resources.stamp
  DEPENDS ${CORE_APP_RESOURCES}
  COMMENT "Copying the app's resources (templates, lang, patterns, pcore)")
add_custom_target(core_resources DEPENDS ${CMAKE_BINARY_DIR}/core_resources.stamp)
add_dependencies(CoreApp core_resources)


# Development package (M1c): a zip with Core.exe, core-engine.exe and the demo, runnable on a Windows
# machine without Node, Python or a network. cmake --build build --target core-package
add_custom_target(core-package
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/package/Core
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:CoreApp> $<TARGET_FILE:core-engine>
          $<TARGET_FILE_DIR:CoreApp>/demo.core-document.json ${CMAKE_CURRENT_SOURCE_DIR}/../README.md
          ${CMAKE_BINARY_DIR}/package/Core
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/package/Core/templates
  COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_FILE_DIR:CoreApp>/templates ${CMAKE_BINARY_DIR}/package/Core/templates
  COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_FILE_DIR:CoreApp>/lang ${CMAKE_BINARY_DIR}/package/Core/lang
  COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_FILE_DIR:CoreApp>/patterns ${CMAKE_BINARY_DIR}/package/Core/patterns
  COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_FILE_DIR:CoreApp>/pcore ${CMAKE_BINARY_DIR}/package/Core/pcore
  COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_BINARY_DIR}/package
          ${CMAKE_COMMAND} -E tar cf ${CMAKE_BINARY_DIR}/Core-windows-x64.zip --format=zip Core
  DEPENDS CoreApp core_resources
  COMMENT "Packing build/Core-windows-x64.zip")
