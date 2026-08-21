cmake_minimum_required(VERSION 3.14)
project(GRMClone VERSION 0.0.3)

add_subdirectory(plugins/BandPass)

# ---- DSP 核心离线自测 (纯 C++17, 不依赖 iPlug2) ----
enable_testing()
add_executable(dsp_test tests/dsp_test.cpp)
target_compile_features(dsp_test PRIVATE cxx_std_17)
add_test(NAME dsp_test COMMAND dsp_test)
