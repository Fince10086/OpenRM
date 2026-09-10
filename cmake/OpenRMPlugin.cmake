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
#   2. APPLE: ad-hoc 签名 -> 部署到 ~/Library/Audio/Plug-Ins/... (POST_BUILD);
#      独立 App 拷字体后部署到 ~/Applications (POST_BUILD, 不加签名)
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

  # 三插件共用头 (plugins/common)
  set(_orm_common_dir "${CMAKE_CURRENT_SOURCE_DIR}/../common")
  list(APPEND _p_SOURCES
    "${_orm_common_dir}/Theme.h"
    "${_orm_common_dir}/controls/SectionTitleControl.h"
    "${_orm_common_dir}/controls/ThemeCornerResizer.h"
  )

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

  # 独立 App 链接时不加 ad-hoc 签名 (macOS 15+ Gatekeeper 对"ad-hoc 签名 + 网络下载"
  # 一律报"已损坏"; 完全无签名才会显示"无法验证开发者", 用户可右键打开)。
  if(APPLE AND TARGET ${NAME}-app)
    target_link_options(${NAME}-app PRIVATE "-Wl,-no_adhoc_codesign")
  endif()

  if(WIN32)
    # Windows: 让 VST3 dll 也编译 main.rc 以嵌入字体与资源。
    set(_rc_file "${CMAKE_CURRENT_SOURCE_DIR}/resources/main.rc")
    if(EXISTS "${_rc_file}" AND TARGET ${NAME}-vst3)
      target_sources(${NAME}-vst3 PRIVATE "${_rc_file}")
      set_source_files_properties("${_rc_file}" PROPERTIES
        COMPILE_FLAGS "/I\"${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts\" /I\"${CMAKE_CURRENT_SOURCE_DIR}/resources/img\" /I\"${CMAKE_CURRENT_SOURCE_DIR}/resources\""
      )
    endif()
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
    set(_app_dst "$ENV{HOME}/Applications/${NAME}.app")
    set(_copy_cmds)
    foreach(_font IN LISTS _p_FONTS)
      list(APPEND _copy_cmds COMMAND ${CMAKE_COMMAND} -E copy "${_p_FONT_DIR}/${_font}" "${_sig_app}/Contents/Resources/")
    endforeach()
    # iPlug2 的 bundle 资源 (主菜单 nib / 图标 icns) 走 MACOSX_PACKAGE_LOCATION
    # 复制, 在 Makefile 生成器上这些复制发生在 POST_BUILD 之后, 且本步骤会
    # rm -rf 整个 Resources 目录, 因此必须在部署前从源文件补拷 nib/icns,
    # 否则落到 ~/Applications 的副本会缺主菜单和图标。
    set(_app_extra_res_cmds)
    set(_app_nib "${CMAKE_CURRENT_BINARY_DIR}/${NAME}-macOS-MainMenu.nib")
    set(_app_xib "${CMAKE_CURRENT_SOURCE_DIR}/resources/${NAME}-macOS-MainMenu.xib")
    if(EXISTS "${_app_xib}")
      # 若复制先于链接执行, 增量构建时 nib 不会被恢复,
      # 把 nib 声明为链接依赖, 部署前即可无条件补拷。
      set_property(TARGET ${NAME}-app APPEND PROPERTY LINK_DEPENDS "${_app_nib}")
      list(APPEND _app_extra_res_cmds
        COMMAND ${CMAKE_COMMAND} -E copy "${_app_nib}" "${_sig_app}/Contents/Resources/")
    endif()
    set(_app_icns "${CMAKE_CURRENT_SOURCE_DIR}/resources/${NAME}.icns")
    if(EXISTS "${_app_icns}")
      list(APPEND _app_extra_res_cmds COMMAND ${CMAKE_COMMAND} -E copy "${_app_icns}" "${_sig_app}/Contents/Resources/")
    endif()
    # 拷字体 -> 部署到用户 Applications。挂 POST_BUILD 保证 `--target ...-app`
    # 单目标构建时也生效 (与上方 AU / VST3 一致)。不做 ad-hoc 签名,
    # 与文件头部 "独立 App 链接时不加 ad-hoc 签名" 的约定保持一致。
    add_custom_command(TARGET ${NAME}-app POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E rm -rf "${_sig_app}/Contents/Resources"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_sig_app}/Contents/Resources"
      ${_copy_cmds}
      ${_app_extra_res_cmds}
      COMMAND ${CMAKE_COMMAND} -E rm -rf "${_app_dst}"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${_sig_app}" "${_app_dst}"
      COMMENT "Copy fonts + deploy ${_sig_app} -> ${_app_dst}"
    )
    add_custom_target(${NAME}-app-sign ALL
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_sig_app}/Contents/Resources"
      ${_copy_cmds}
      DEPENDS ${NAME}-app
      COMMENT "Copy fonts ${_sig_app}"
    )
  endif()
endfunction()
