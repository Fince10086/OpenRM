cmake_minimum_required(VERSION 3.14)

# 版本单一来源: config.h (PLUG_VERSION_STR)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/config.h" _az_version REGEX "PLUG_VERSION_STR \"[0-9.]+\"")
string(REGEX MATCH "\"([0-9.]+)\"" _az_version_match "${_az_version}")
project(ORMAnalyzer VERSION ${CMAKE_MATCH_1})

# iPlug2 初始化必须在目录作用域 (内含 enable_language(OBJC/OBJCXX))
if(NOT DEFINED IPLUG2_DIR)
  set(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/iPlug2" CACHE PATH "iPlug2 root directory")
endif()
include(${IPLUG2_DIR}/iPlug2.cmake)
find_package(iPlug2 REQUIRED)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
include(OpenRMPlugin)

openrm_add_plugin(${PROJECT_NAME}
  SOURCES
    src/Analyzer.h
    src/Analyzer.cpp
    src/Params.h
    src/Theme.h
    src/Strings.h
    src/dsp/SpectrumSTFT.h
    src/dsp/VQTAnalyzer.h
    src/dsp/PAZAnalyzer.h
    src/dsp/LevelMeter.h
    src/controls/UiUtils.h
    src/controls/ThemeCornerResizer.h
    src/controls/FlatButton.h
    src/controls/ORMSlider.h
    src/controls/SectionTitleControl.h
    src/controls/SettingsPanelControl.h
    src/controls/SpectrumPad.h
    src/StateFileIO.h
    src/SettingsFileIO.h
    ${IPLUG2_DIR}/WDL/fft.c
  FORMATS APP AU VST3
  DEFINES SAMPLE_TYPE_FLOAT
  FONTS Mixed-Regular.ttf Mixed-SemiBold.ttf Mixed-Bold.ttf
)
