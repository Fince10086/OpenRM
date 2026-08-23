cmake_minimum_required(VERSION 3.14)
project(OpenRM VERSION 0.2.0)

# 右键留给插件自己的菜单（映射色块换色等），不弹宿主参数菜单
add_compile_definitions(IGRAPHICS_NO_CONTEXT_MENU)

add_subdirectory(plugins/BandPass)

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
