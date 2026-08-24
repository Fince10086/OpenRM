cmake_minimum_required(VERSION 3.14)

# 版本单一来源: plugins/BandPass/config.h (PLUG_VERSION_STR)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/plugins/BandPass/config.h" _orm_version REGEX "PLUG_VERSION_STR \"[0-9.]+\"")
string(REGEX MATCH "\"([0-9.]+)\"" _orm_version_match "${_orm_version}")
project(OpenRM VERSION ${CMAKE_MATCH_1})

# macOS 最低系统版本: 11.0

if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

# 右键留给插件自己的菜单（映射色块换色等），不弹宿主参数菜单
add_compile_definitions(IGRAPHICS_NO_CONTEXT_MENU)

add_subdirectory(plugins/BandPass)
add_subdirectory(plugins/Analyzer)

# VST3 SDK 缺失时 iPlug2 只会静默跳过目标, 这里给出明确提示
if(TARGET ORMBandPass-vst3)
  get_target_property(_vst3_excluded ORMBandPass-vst3 EXCLUDE_FROM_ALL)
  if(_vst3_excluded)
    message(WARNING "未找到 VST3 SDK, 已跳过 VST3 目标 (APP/AU 不受影响)。首次构建请先运行: ./scripts/get_vst3_sdk.sh")
  endif()
endif()

# ---- DSP 核心离线自测 (纯 C++17, 不依赖 iPlug2) ----
enable_testing()
add_executable(dsp_test tests/dsp_test.cpp)
target_compile_features(dsp_test PRIVATE cxx_std_17)
add_test(NAME dsp_test COMMAND dsp_test)
