cmake_minimum_required(VERSION 3.14)

# 版本单一来源: config.h (PLUG_VERSION_STR)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/config.h" _nr_version REGEX "PLUG_VERSION_STR \"[0-9.]+\"")
string(REGEX MATCH "\"([0-9.]+)\"" _nr_version_match "${_nr_version}")
project(ORMNarrator VERSION ${CMAKE_MATCH_1})

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
    src/Narrator.h
    src/Narrator.cpp
    src/Params.h
    src/Theme.h
    src/Strings.h
    src/dsp/SamEngine.h
    src/dsp/SamEngine.cpp
    src/dsp/Tms5220Engine.h
    src/dsp/Tms5220Engine.cpp
    src/dsp/Tms5110Engine.h
    src/dsp/Tms5110Engine.cpp
    src/dsp/TsiS14001Engine.h
    src/dsp/TsiS14001Engine.cpp
    src/dsp/Sp0256Engine.h
    src/dsp/Sp0256Engine.cpp
    src/dsp/s14001/S14001Roms.h
    src/dsp/sp0256/Sp0256Roms.h
    src/dsp/tms/TmsWords.h
    src/dsp/tms/TmsVocab.h
    src/dsp/tms/VocabTI99.h
    src/dsp/tms/VocabAcorn.h
    src/dsp/tms/VocabClock.h
    src/dsp/tms/VocabSspell.h
    src/dsp/sam/sam.c
    src/dsp/sam/render.c
    src/dsp/sam/reciter.c
    src/dsp/sam/ReciterTabs.h
    src/dsp/sam/RenderTabs.h
    src/dsp/sam/SamTabs.h
    src/dsp/sam/sam.h
    src/dsp/sam/reciter.h
    src/dsp/sam/render.h
    src/dsp/sam/debug.h
    src/dsp/VoiceRenderer.h
    src/controls/UiUtils.h
    src/controls/ThemeCornerResizer.h
    src/controls/FlatButton.h
    src/controls/ORMSlider.h
    src/controls/SectionTitleControl.h
    src/controls/SettingsPanelControl.h
    src/controls/PianoKeyboardControl.h
    src/controls/PhraseEditorControl.h
    src/controls/UtteranceTimelineControl.h
    src/SettingsFileIO.h
  FORMATS APP AU VST3
  DEFINES SAMPLE_TYPE_FLOAT
  FONTS Mixed-Regular.ttf Mixed-SemiBold.ttf Mixed-Bold.ttf
)

# macOS 渲染节拍改由 CVDisplayLink (vsync) 驱动 (与 Analyzer 对齐):
# 默认自由运行的 60Hz NSTimer 与 CAMetalLayer 的 vsync present 双钟漂移,
# 拖拽缩放时鼠标事件持续踢相位导致帧间隔震荡, 窗口服务器拉伸旧帧 → 文字发糊。
# 宏使用处 IGraphicsMac_view.mm 直接编进各格式目标, 因此逐目标注入。
if(APPLE)
  foreach(_nr_fmt_tgt ${PROJECT_NAME}-app ${PROJECT_NAME}-vst3 ${PROJECT_NAME}-au)
    if(TARGET ${_nr_fmt_tgt})
      target_compile_definitions(${_nr_fmt_tgt} PRIVATE IGRAPHICS_CVDISPLAYLINK)
    endif()
  endforeach()
endif()
