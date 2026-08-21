cmake_minimum_required(VERSION 3.14)
project(GRMBandPass VERSION 0.0.2)

# 定位 iPlug2 (默认相对本文件路径, 可用 -DIPLUG2_DIR=... 覆盖)
if(NOT DEFINED IPLUG2_DIR)
  set(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/iPlug2" CACHE PATH "iPlug2 root directory")
endif()

include(${IPLUG2_DIR}/iPlug2.cmake)
find_package(iPlug2 REQUIRED)

# AU (macOS 原生) + APP (独立运行, 便于快速调试) + VST3 (需 Steinberg VST3 SDK)
# VST3_SDK 已就位: third_party/iPlug2/Dependencies/IPlug/VST3_SDK
# 拿到 CLAP_SDK 后可追加 CLAP: FORMATS APP AU VST3 CLAP
iplug_add_plugin(${PROJECT_NAME}
  SOURCES
    src/BandPass.h
    src/BandPass.cpp
    src/dsp/BandPassCore.h
    src/controls/FilterNodePad.h
  RESOURCES
    resources/fonts/Outfit-Regular.ttf
    resources/fonts/Outfit-SemiBold.ttf
    resources/fonts/Outfit-Bold.ttf
  FORMATS APP AU VST3
  DEFINES SAMPLE_TYPE_FLOAT
)
