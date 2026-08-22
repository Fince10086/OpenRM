cmake_minimum_required(VERSION 3.14)
project(ORMBandPass VERSION 0.1.1)

# 定位 iPlug2 (默认相对本文件路径, 可用 -DIPLUG2_DIR=... 覆盖)
if(NOT DEFINED IPLUG2_DIR)
  set(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/iPlug2" CACHE PATH "iPlug2 root directory")
endif()

include(${IPLUG2_DIR}/iPlug2.cmake)
find_package(iPlug2 REQUIRED)

# 合成字体: 编译时按 Strings.h 用到的中文, 将 OPPO Sans(实例化字重+子集化)
# 与对应字重的 Outfit 合成 Mixed-*.ttf (ASCII 用 Outfit 字形, 中文用 OPPO 字形)
# 在 CMake 找到的解释器和 PATH 中的 python3 里挑一个带 fontTools 的
find_package(Python3 COMPONENTS Interpreter QUIET)
set(ORM_PY_CANDIDATES)
if(Python3_EXECUTABLE)
  list(APPEND ORM_PY_CANDIDATES ${Python3_EXECUTABLE})
endif()
find_program(ORM_PYTHON3_PATH python3)
if(ORM_PYTHON3_PATH)
  list(APPEND ORM_PY_CANDIDATES ${ORM_PYTHON3_PATH})
endif()
list(REMOVE_DUPLICATES ORM_PY_CANDIDATES)
set(ORM_PYTHON_SUBSET "")
foreach(_py IN LISTS ORM_PY_CANDIDATES)
  execute_process(COMMAND ${_py} -c "import fontTools" RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
  if(_rc EQUAL 0)
    set(ORM_PYTHON_SUBSET ${_py})
    break()
  endif()
endforeach()
if(NOT ORM_PYTHON_SUBSET)
  message(FATAL_ERROR "合成字体需要带 fontTools 的 Python3, 请执行: pip3 install fonttools")
endif()
message(STATUS "Font subsetter python: ${ORM_PYTHON_SUBSET}")
set(MIXED_FONT_DIR ${CMAKE_CURRENT_BINARY_DIR}/fonts)
set(MIXED_FONTS
  ${MIXED_FONT_DIR}/Mixed-Regular.ttf
  ${MIXED_FONT_DIR}/Mixed-SemiBold.ttf
  ${MIXED_FONT_DIR}/Mixed-Bold.ttf
)
add_custom_command(
  OUTPUT ${MIXED_FONTS}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MIXED_FONT_DIR}
  COMMAND ${ORM_PYTHON_SUBSET} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/subset_font.py
          ${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/OPPOSans-Source.ttf
          ${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts
          ${CMAKE_CURRENT_SOURCE_DIR}/src/Strings.h
          ${MIXED_FONT_DIR}
  DEPENDS resources/fonts/OPPOSans-Source.ttf
          resources/fonts/Outfit-Regular.ttf
          resources/fonts/Outfit-SemiBold.ttf
          resources/fonts/Outfit-Bold.ttf
          src/Strings.h scripts/subset_font.py
  COMMENT "Merging Outfit + OPPO Sans (Chinese UI glyphs)"
  VERBATIM
)

# AU (macOS 原生) + APP (独立运行, 便于快速调试) + VST3 (需 Steinberg VST3 SDK)
# VST3_SDK 已就位: third_party/iPlug2/Dependencies/IPlug/VST3_SDK
# 拿到 CLAP_SDK 后可追加 CLAP: FORMATS APP AU VST3 CLAP
iplug_add_plugin(${PROJECT_NAME}
  SOURCES
    src/BandPass.h
    src/BandPass.cpp
    src/Theme.h
    src/Strings.h
    src/dsp/BandPassCore.h
    src/dsp/SpectrumSTFT.h
    src/controls/FilterNodePad.h
    src/controls/BandRangeSlider.h
    src/controls/PresetSlotControl.h
    src/PresetFileIO.h
    ${IPLUG2_DIR}/WDL/fft.c
  RESOURCES
    resources/fonts/Outfit-Regular.ttf
    resources/fonts/Outfit-SemiBold.ttf
    resources/fonts/Outfit-Bold.ttf
    ${MIXED_FONTS}
  FORMATS APP AU VST3
  DEFINES SAMPLE_TYPE_FLOAT
)

# nlohmann/json (iPlug2 vendored) 不在默认 include path, 补一行供 PresetFileIO.h 使用
target_include_directories(_${PROJECT_NAME}-base INTERFACE "${IPLUG2_DIR}/Dependencies/Extras")
