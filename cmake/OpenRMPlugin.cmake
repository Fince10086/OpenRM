# OpenRM 系列插件构建函数。
#
# 用法 (每个插件的 CMakeLists.txt):
#   if(NOT DEFINED IPLUG2_DIR)
#     set(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/iPlug2" CACHE PATH "iPlug2 root directory")
#   endif()
#   include(${IPLUG2_DIR}/iPlug2.cmake)   # 必须在目录作用域调用: 内含 enable_language(OBJC/OBJCXX)
#   find_package(iPlug2 REQUIRED)
#   list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
#   include(OpenRMPlugin)
#   openrm_add_plugin(${PROJECT_NAME}
#     SOURCES src/Plugin.cpp ...
#     FORMATS APP AU VST3
#     DEFINES SAMPLE_TYPE_FLOAT
#     FONTS Mixed-Regular.ttf Mixed-SemiBold.ttf Mixed-Bold.ttf   # 可选: 需要打包的字体
#     FONT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts        # 可选, 默认如上
#   )
#
# 封装内容:
#   1. iplug_add_plugin 目标创建; FONTS 展开为完整路径走 RESOURCES 参数:
#      - Windows: 字体嵌入 dll (IGraphics Win 端 LocateResource 优先读嵌入 TTF 资源)
#      - macOS:   字体进 bundle Contents/Resources (与下方 POST_BUILD 签名/部署互补)
#   2. APPLE: ad-hoc 签名 -> 部署到 ~/Library/Audio/Plug-Ins/... (POST_BUILD)
#   3. Extras include 目录 (nlohmann/json 等)
#
# 注意: iPlug2.cmake 的 include 与 find_package 必须留在插件 CMakeLists 目录作用域,
#        (其内部 enable_language(OBJC/OBJCXX) 在函数作用域调用会导致生成阶段缺
#        CMAKE_OBJCXX_COMPILE_OBJECT 规则)。本函数只负责目标创建与部署样板。

function(openrm_add_plugin NAME)
  cmake_parse_arguments(_p "" "FONT_DIR" "SOURCES;DEFINES;FORMATS;FONTS" ${ARGN})

  if(NOT _p_FONT_DIR)
    set(_p_FONT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts")
  endif()

  # 字体展开为完整路径, 经 RESOURCES 交给 iPlug2:
  #   Windows: 嵌入 dll 的 TTF 资源 (LocateResource 优先读取, 无需拷贝文件)
  #   macOS:   进 bundle Contents/Resources
  set(_font_resources)
  foreach(_font IN LISTS _p_FONTS)
    list(APPEND _font_resources "${_p_FONT_DIR}/${_font}")
  endforeach()

  iplug_add_plugin(${NAME}
    SOURCES ${_p_SOURCES}
    RESOURCES ${_font_resources}
    FORMATS ${_p_FORMATS}
    DEFINES ${_p_DEFINES}
  )

  # nlohmann/json 等第三方工具头 (PresetFileIO 使用)
  target_include_directories(_${NAME}-base INTERFACE "${IPLUG2_DIR}/Dependencies/Extras")

  if(NOT APPLE)
    return()
  endif()

  # 注意: 字体已通过上方 RESOURCES 参数进入 bundle (macOS) / 嵌入二进制 (Windows)。
  # 这里在 mac 上重建 Resources 目录并重新拷贝字体, 再执行 ad-hoc 签名 -> 部署,
  # 保证单目标 (--target ...) 构建时签名步骤也生效。
  find_program(IPLUG2_CODESIGN_EXEC codesign)
  if(NOT IPLUG2_CODESIGN_EXEC OR NOT _p_FONTS)
    return()
  endif()

  if(TARGET ${NAME}-au AND TARGET ${NAME}-vst3)
    set(_sig_src_au  "${CMAKE_BINARY_DIR}/out/${NAME}.component")
    set(_sig_dst_au  "$ENV{HOME}/Library/Audio/Plug-Ins/Components/${NAME}.component")
    set(_sig_src_v3  "${CMAKE_BINARY_DIR}/out/${NAME}.vst3")
    set(_sig_dst_v3  "$ENV{HOME}/Library/Audio/Plug-Ins/VST3/${NAME}.vst3")

    # AU / VST3: copy fonts -> ad-hoc sign -> redeploy.
    # Attached as POST_BUILD so single-target (`--target ...`) builds sign too.
    foreach(_sig_tgt ${NAME}-au ${NAME}-vst3)
      if(_sig_tgt MATCHES "-au$")
        set(_sig_src "${_sig_src_au}")
        set(_sig_dst "${_sig_dst_au}")
      else()
        set(_sig_src "${_sig_src_v3}")
        set(_sig_dst "${_sig_dst_v3}")
      endif()
      set(_copy_cmds)
      foreach(_font IN LISTS _p_FONTS)
        list(APPEND _copy_cmds COMMAND ${CMAKE_COMMAND} -E copy "${_p_FONT_DIR}/${_font}" "${_sig_src}/Contents/Resources/")
      endforeach()
      add_custom_command(TARGET ${_sig_tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${_sig_src}/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_sig_src}/Contents/Resources"
        ${_copy_cmds}
        COMMAND ${IPLUG2_CODESIGN_EXEC} --force --deep --sign - "${_sig_src}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${_sig_dst}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_sig_src}" "${_sig_dst}"
        COMMENT "Copy fonts + ad-hoc sign + deploy ${_sig_src}"
      )
    endforeach()
  endif()

  # Standalone app
  if(TARGET ${NAME}-app)
    set(_sig_app "${CMAKE_BINARY_DIR}/out/${NAME}.app")
    set(_copy_cmds)
    foreach(_font IN LISTS _p_FONTS)
      list(APPEND _copy_cmds COMMAND ${CMAKE_COMMAND} -E copy "${_p_FONT_DIR}/${_font}" "${_sig_app}/Contents/Resources/")
    endforeach()
    add_custom_target(${NAME}-app-sign ALL
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_sig_app}/Contents/Resources"
      ${_copy_cmds}
      DEPENDS ${NAME}-app
      COMMENT "Copy fonts ${_sig_app}"
    )
  endif()
endfunction()
