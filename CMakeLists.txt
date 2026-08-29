cmake_minimum_required(VERSION 3.14)

# 构建类型默认 Release (-O3 -DNDEBUG)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Release/Debug/RelWithDebInfo)" FORCE)
  message(STATUS "CMAKE_BUILD_TYPE 未指定, 默认使用 Release (优化构建)")
endif()

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

# 部署统一由 cmake/OpenRMPlugin.cmake 负责 (AU/VST3 -> ~/Library/Audio/Plug-Ins,
# App -> ~/Applications, 且先拷字体再签名/复制)。显式关闭 iPlug2 自带的自动部署
# (IPLUG_DEPLOY_PLUGINS), 否则同一产物会被复制两份, 且 iPlug2 的拷贝先于字体
# 处理执行, 会往目标目录写缺字体的副本。
set(IPLUG_DEPLOY_PLUGINS OFF CACHE BOOL "Deploy handled by OpenRMPlugin.cmake" FORCE)

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

add_executable(loudness_test tests/loudness_test.cpp)
target_compile_features(loudness_test PRIVATE cxx_std_17)
add_test(NAME loudness_test COMMAND loudness_test)
