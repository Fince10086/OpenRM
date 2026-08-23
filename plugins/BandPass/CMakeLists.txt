cmake_minimum_required(VERSION 3.14)
project(ORMBandPass VERSION 0.1.1)

if(NOT DEFINED IPLUG2_DIR)
  set(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/iPlug2" CACHE PATH "iPlug2 root directory")
endif()

include(${IPLUG2_DIR}/iPlug2.cmake)
find_package(iPlug2 REQUIRED)

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
    resources/fonts/Mixed-Regular.ttf
    resources/fonts/Mixed-SemiBold.ttf
    resources/fonts/Mixed-Bold.ttf
  FORMATS APP AU VST3
  DEFINES SAMPLE_TYPE_FLOAT
)

target_include_directories(_${PROJECT_NAME}-base INTERFACE "${IPLUG2_DIR}/Dependencies/Extras")
