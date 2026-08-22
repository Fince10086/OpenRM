cmake_minimum_required(VERSION 3.14)
project(ORMBandPass VERSION 0.1.0)

# 定位 iPlug2 (默认相对本文件路径, 可用 -DIPLUG2_DIR=... 覆盖)
if(NOT DEFINED IPLUG2_DIR)
  set(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/iPlug2" CACHE PATH "iPlug2 root directory")
endif()

include(${IPLUG2_DIR}/iPlug2.cmake)
find_package(iPlug2 REQUIRED)

# OPPO Sans 中文子集字体: 编译时按 Strings.h 实际用到的字符自动子集化(含 ASCII),
# 并从可变字体实例化出三个字重, 与 Outfit 的 Regular/SemiBold/Bold 一一对应
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
  message(FATAL_ERROR "OPPOSans 子集化需要带 fontTools 的 Python3, 请执行: pip3 install fonttools")
endif()
message(STATUS "Font subsetter python: ${ORM_PYTHON_SUBSET}")
set(OPPO_SANS_DIR ${CMAKE_CURRENT_BINARY_DIR}/fonts)
set(OPPO_SANS_SUBSETS
  ${OPPO_SANS_DIR}/OPPOSans-ZH-Regular.ttf
  ${OPPO_SANS_DIR}/OPPOSans-ZH-SemiBold.ttf
  ${OPPO_SANS_DIR}/OPPOSans-ZH-Bold.ttf
)
add_custom_command(
  OUTPUT ${OPPO_SANS_SUBSETS}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${OPPO_SANS_DIR}
  COMMAND ${ORM_PYTHON_SUBSET} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/subset_font.py
          ${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/OPPOSans-Source.ttf
          ${CMAKE_CURRENT_SOURCE_DIR}/src/Strings.h
          ${OPPO_SANS_DIR}
  DEPENDS resources/fonts/OPPOSans-Source.ttf src/Strings.h scripts/subset_font.py
  COMMENT "Subsetting OPPO Sans (Chinese UI glyphs)"
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
    src/controls/FilterNodePad.h
    src/controls/BandRangeSlider.h
    src/controls/PresetSlotControl.h
    src/PresetFileIO.h
  RESOURCES
    resources/fonts/Outfit-Regular.ttf
    resources/fonts/Outfit-SemiBold.ttf
    resources/fonts/Outfit-Bold.ttf
    ${OPPO_SANS_SUBSETS}
  FORMATS APP AU VST3
  DEFINES SAMPLE_TYPE_FLOAT
)

# nlohmann/json (iPlug2 vendored) 不在默认 include path, 补一行供 PresetFileIO.h 使用
target_include_directories(_${PROJECT_NAME}-base INTERFACE "${IPLUG2_DIR}/Dependencies/Extras")
