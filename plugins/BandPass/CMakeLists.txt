cmake_minimum_required(VERSION 3.14)

# 版本单一来源: config.h (PLUG_VERSION_STR)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/config.h" _bp_version REGEX "PLUG_VERSION_STR \"[0-9.]+\"")
string(REGEX MATCH "\"([0-9.]+)\"" _bp_version_match "${_bp_version}")
project(ORMBandPass VERSION ${CMAKE_MATCH_1})

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
    src/BandPass.h
    src/BandPass.cpp
    src/Params.h
    src/Theme.h
    src/Strings.h
    src/dsp/BandPassCore.h
    src/dsp/SpectrumSTFT.h
    src/controls/UiUtils.h
    src/controls/ThemeCornerResizer.h
    src/controls/FlatButton.h
    src/controls/ORMSlider.h
    src/controls/GainSlider.h
    src/controls/PresetFadeSlider.h
    src/controls/SectionTitleControl.h
    src/controls/RandomColorPickerControl.h
    src/controls/SettingsPanelControl.h
    src/controls/FilterNodePad.h
    src/controls/BandRangeSlider.h
    src/controls/PresetSlotControl.h
    src/PresetFileIO.h
    src/SettingsFileIO.h
    ${IPLUG2_DIR}/WDL/fft.c
  FORMATS APP AU VST3
  DEFINES SAMPLE_TYPE_FLOAT
  FONTS Mixed-Regular.ttf Mixed-SemiBold.ttf Mixed-Bold.ttf
)

# macOS 渲染节拍改由 CVDisplayLink (vsync) 驱动 (与 Analyzer 对齐):
# 默认自由运行的 60Hz NSTimer 与 CAMetalLayer 的 vsync present 双钟漂移,
# 拖拽缩放时鼠标事件持续踢相位导致帧间隔震荡, 窗口服务器拉伸旧帧 → 文字发糊。
# 宏使用处 IGraphicsMac_view.mm 直接编进各格式目标, 因此逐目标注入。
if(APPLE)
  foreach(_bp_fmt_tgt ${PROJECT_NAME}-app ${PROJECT_NAME}-vst3 ${PROJECT_NAME}-au)
    if(TARGET ${_bp_fmt_tgt})
      target_compile_definitions(${_bp_fmt_tgt} PRIVATE IGRAPHICS_CVDISPLAYLINK)
    endif()
  endforeach()
endif()
