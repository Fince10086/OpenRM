cmake_minimum_required(VERSION 3.14)
project(ORMBandPass VERSION 0.3.0)

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
  FORMATS APP AU VST3
  DEFINES SAMPLE_TYPE_FLOAT
)

# 注意：字体故意不通过 iPlug2 的 RESOURCES 参数打包，
# 在下方 POST_BUILD 中按“拷字体 -> 签名 -> 部署”顺序手动处理。

target_include_directories(_${PROJECT_NAME}-base INTERFACE "${IPLUG2_DIR}/Dependencies/Extras")

if(APPLE)
  find_program(IPLUG2_CODESIGN_EXEC codesign)
  set(_font_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts")

  if(IPLUG2_CODESIGN_EXEC AND TARGET ${PROJECT_NAME}-au AND TARGET ${PROJECT_NAME}-vst3)
    set(_sig_src_au  "${CMAKE_BINARY_DIR}/out/${PROJECT_NAME}.component")
    set(_sig_dst_au  "$ENV{HOME}/Library/Audio/Plug-Ins/Components/${PROJECT_NAME}.component")
    set(_sig_src_v3  "${CMAKE_BINARY_DIR}/out/${PROJECT_NAME}.vst3")
    set(_sig_dst_v3  "$ENV{HOME}/Library/Audio/Plug-Ins/VST3/${PROJECT_NAME}.vst3")

    # 1) AU / VST3: copy fonts -> ad-hoc sign -> redeploy.
    #    Attached as POST_BUILD so single-target (`--target ...`) builds sign too.
    foreach(_sig_tgt ${PROJECT_NAME}-au ${PROJECT_NAME}-vst3)
      if(_sig_tgt MATCHES "-au$")
        set(_sig_src "${_sig_src_au}")
        set(_sig_dst "${_sig_dst_au}")
      else()
        set(_sig_src "${_sig_src_v3}")
        set(_sig_dst "${_sig_dst_v3}")
      endif()
      add_custom_command(TARGET ${_sig_tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${_sig_src}/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_sig_src}/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy "${_font_src_dir}/Mixed-Regular.ttf"    "${_sig_src}/Contents/Resources/Mixed-Regular.ttf"
        COMMAND ${CMAKE_COMMAND} -E copy "${_font_src_dir}/Mixed-SemiBold.ttf"   "${_sig_src}/Contents/Resources/Mixed-SemiBold.ttf"
        COMMAND ${CMAKE_COMMAND} -E copy "${_font_src_dir}/Mixed-Bold.ttf"       "${_sig_src}/Contents/Resources/Mixed-Bold.ttf"
        COMMAND ${IPLUG2_CODESIGN_EXEC} --force --deep --sign - "${_sig_src}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${_sig_dst}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_sig_src}" "${_sig_dst}"
        COMMENT "Copy fonts + ad-hoc sign + deploy ${_sig_src}"
      )
    endforeach()
  endif()

  # 2) Standalone app
  if(IPLUG2_CODESIGN_EXEC AND TARGET ${PROJECT_NAME}-app)
    set(_sig_app "${CMAKE_BINARY_DIR}/out/${PROJECT_NAME}.app")
    add_custom_target(${PROJECT_NAME}-app-sign ALL
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_sig_app}/Contents/Resources"
      COMMAND ${CMAKE_COMMAND} -E copy "${_font_src_dir}/Mixed-Regular.ttf"    "${_sig_app}/Contents/Resources/Mixed-Regular.ttf"
      COMMAND ${CMAKE_COMMAND} -E copy "${_font_src_dir}/Mixed-SemiBold.ttf"   "${_sig_app}/Contents/Resources/Mixed-SemiBold.ttf"
      COMMAND ${CMAKE_COMMAND} -E copy "${_font_src_dir}/Mixed-Bold.ttf"       "${_sig_app}/Contents/Resources/Mixed-Bold.ttf"
      COMMAND ${IPLUG2_CODESIGN_EXEC} --force --deep --sign - "${_sig_app}"
      DEPENDS ${PROJECT_NAME}-app
      COMMENT "Copy fonts + ad-hoc sign ${_sig_app}"
    )
  endif()
endif()
