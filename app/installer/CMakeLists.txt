# Core Setup.exe (M6): one file that installs Core for the current user (no administrator rights).
# The package (Core.exe, core-engine.exe, the demo, the templates, the languages, the patterns, PCore's styles) is built here and carried inside it.
#   cmake --build build --target CoreSetup   ->  build/app/installer/CoreSetup_artefacts/Release/Core Setup.exe
set(CORE_STAGE ${CMAKE_CURRENT_BINARY_DIR}/stage)
set(CORE_PACKAGE_ZIP ${CMAKE_CURRENT_BINARY_DIR}/CorePackage.zip)
# Audit 29: the zip carries the repo's resources, so it rebuilds when they change, not only when the .exe does.
file(GLOB_RECURSE CORE_PACKAGE_RESOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/templates/* ${CMAKE_SOURCE_DIR}/lang/* ${CMAKE_SOURCE_DIR}/patterns/* ${CMAKE_SOURCE_DIR}/pcore/*)
add_custom_command(OUTPUT ${CORE_PACKAGE_ZIP}
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CORE_STAGE}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CORE_STAGE}
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:CoreApp> $<TARGET_FILE:core-engine> $<TARGET_FILE_DIR:CoreApp>/demo.core-document.json ${CORE_STAGE}
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/templates ${CORE_STAGE}/templates
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/lang ${CORE_STAGE}/lang
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/patterns ${CORE_STAGE}/patterns
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/pcore ${CORE_STAGE}/pcore
  COMMAND ${CMAKE_COMMAND} -E chdir ${CORE_STAGE} ${CMAKE_COMMAND} -E tar cf ${CORE_PACKAGE_ZIP} --format=zip
          Core.exe core-engine.exe demo.core-document.json templates lang patterns pcore
  DEPENDS CoreApp core-engine ${CMAKE_SOURCE_DIR}/app/desktop/demo.core-document.json
          ${CORE_PACKAGE_RESOURCES}
  COMMENT "Packing the Core package for the installer")
juce_add_binary_data(CoreSetupData HEADER_NAME CoreSetupData.h NAMESPACE CoreSetupData SOURCES ${CORE_PACKAGE_ZIP})

juce_add_gui_app(CoreSetup
  PRODUCT_NAME "Core Setup"
  COMPANY_NAME "Core"
  VERSION ${CORE_VERSION}
  NEEDS_WEB_BROWSER FALSE
  NEEDS_CURL FALSE)
target_sources(CoreSetup PRIVATE Setup.cpp)
target_compile_definitions(CoreSetup PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:CoreApp,JUCE_VERSION>")  # the version of Core it installs
target_link_libraries(CoreSetup PRIVATE CoreSetupData juce::juce_gui_basics PUBLIC juce::juce_recommended_config_flags)

