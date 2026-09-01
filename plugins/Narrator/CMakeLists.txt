cmake_minimum_required(VERSION 3.14)

# 版本单一来源: config.h (PLUG_VERSION_STR)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/config.h" _nr_version REGEX "PLUG_VERSION_STR \"[0-9.]+\"")
string(REGEX MATCH "\"([0-9.]+)\"" _nr_version_match "${_nr_version}")
project(ORMNarrator VERSION ${CMAKE_MATCH_1})

# iPlug2 初始化必须在目录作用域 (内含 enable_language(OBJC/OBJCXX))
if(NOT DEFINED IPLUG2_DIR)
  set(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/iPlug2" CACHE PATH "iPlug2 root directory")
endif()
include(${IPLUG2_DIR}/iPlug2.cmake)
find_package(iPlug2 REQUIRED)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
include(OpenRMPlugin)

openrm_add_plugin(${PROJECT_NAME}
  SOURCES
    src/Narrator.h
    src/Narrator.cpp
    src/Params.h
    src/Theme.h
    src/Strings.h
    src/dsp/SamEngine.h
    src/dsp/SamEngine.cpp
    src/dsp/Tms5220Engine.h
    src/dsp/Tms5220Engine.cpp
    src/dsp/Tms5110Engine.h
    src/dsp/Tms5110Engine.cpp
    src/dsp/TsiS14001Engine.h
    src/dsp/TsiS14001Engine.cpp
    src/dsp/Sp0256Engine.h
    src/dsp/Sp0256Engine.cpp
    src/dsp/Cts256aEngine.h
    src/dsp/Cts256aEngine.cpp
    src/dsp/cts256a/Tms7000Cpu.h
    src/dsp/cts256a/Tms7000Cpu.cpp
    src/dsp/cts256a/Cts256aRoms.h
    src/dsp/s14001/S14001Roms.h
    src/dsp/sp0256/Sp0256Roms.h
    src/dsp/tms/TmsWords.h
    src/dsp/tms/TmsVocab.h
    src/dsp/tms/VocabTI99.h
    src/dsp/tms/VocabAcorn.h
    src/dsp/tms/VocabClock.h
    src/dsp/tms/VocabSspell.h
    src/dsp/sam/sam.c
    src/dsp/sam/render.c
    src/dsp/sam/reciter.c
    src/dsp/sam/ReciterTabs.h
    src/dsp/sam/RenderTabs.h
    src/dsp/sam/SamTabs.h
    src/dsp/sam/sam.h
    src/dsp/sam/reciter.h
    src/dsp/sam/render.h
    src/dsp/sam/debug.h
    src/dsp/DectalkEngine.h
    src/dsp/DectalkEngine.cpp
    src/dsp/VoiceRenderer.h
    src/controls/UiUtils.h
    src/controls/ThemeCornerResizer.h
    src/controls/FlatButton.h
    src/controls/ORMSlider.h
    src/controls/SectionTitleControl.h
    src/controls/SettingsPanelControl.h
    src/controls/PianoKeyboardControl.h
    src/controls/PhraseEditorControl.h
    src/controls/UtteranceTimelineControl.h
    src/SettingsFileIO.h
  FORMATS APP AU VST3
  DEFINES SAMPLE_TYPE_FLOAT
  FONTS Mixed-Regular.ttf Mixed-SemiBold.ttf Mixed-Bold.ttf
)

# macOS 渲染节拍改由 CVDisplayLink (vsync) 驱动 (与 Analyzer 对齐):
# 默认自由运行的 60Hz NSTimer 与 CAMetalLayer 的 vsync present 双钟漂移,
# 拖拽缩放时鼠标事件持续踢相位导致帧间隔震荡, 窗口服务器拉伸旧帧 → 文字发糊。
# 宏使用处 IGraphicsMac_view.mm 直接编进各格式目标, 因此逐目标注入。
if(APPLE)
  foreach(_nr_fmt_tgt ${PROJECT_NAME}-app ${PROJECT_NAME}-vst3 ${PROJECT_NAME}-au)
    if(TARGET ${_nr_fmt_tgt})
      target_compile_definitions(${_nr_fmt_tgt} PRIVATE IGRAPHICS_CVDISPLAYLINK)
    endif()
  endforeach()
endif()

# ---- DECTalk 引擎 (dectalk/ 移植, 来源与许可见 dectalk/README.md) ----
# 需要 DECtalk 特有编译宏与头目录的只有 dectalk/src/*.c (HLSyn/文本分析内核),
# 编译为独立 OBJECT 库后按 $<TARGET_OBJECTS> 并入各格式目标, 宏不污染插件其余
# 代码。宏配置与 DECtalkMini 的 CMake 一致 (另开 NO_FILESYSTEM/NO_CHARSET:
# 词典编译进二进制、绕过 macOS charset 透传分支缺陷)。
#
# 显式白名单而非 src/*.c GLOB: vendored 树的 87 个 .c 里只有 55 个真正必需。
# 排除依据为实测 (对象级链接瘦身 + 全音色/音素/语速/音高输出逐字节一致):
#  1) 19 个运行期零引用的死代码 — 其中 frame/hlframe/inithl/llinit/reson/sample/
#     acxf1c/circuit/nasalf1x/log10table/sqrttable/brent 是老式 Frame 合成器,
#     本平台输出走 VTM (vtm.c/vtmiont.c) 不经过它; 另有 crypt2 (FONIX 授权加密)、
#     dtmmio (Windows MMIO)、dbgwins (调试窗口)、phinit、playstub、decstd97、voice。
#  2) 13 个在当前宏下编译为空文件 — 非英语 LTS 表 (lsa_fr/gr/ir/it/ja/sl/sp)、
#     maindict_be (大端词典孪生, 33k 行, 小端时整文件 #ifdef 排除)、charset
#     (NO_CHARSET)、loadable (NO_FILESYSTEM)、mmalloc、par_ambi、ph_syntx。
# 文件保留在树中 (便于追溯/将来启用), 只是不再参与构建。
set(_nr_dectalk_srcs
  src/dsp/dectalk/src/cm_char.c
  src/dsp/dectalk/src/cm_cmd.c
  src/dsp/dectalk/src/cm_copt.c
  src/dsp/dectalk/src/cm_main.c
  src/dsp/dectalk/src/cm_pars.c
  src/dsp/dectalk/src/cm_phon.c
  src/dsp/dectalk/src/cm_text.c
  src/dsp/dectalk/src/cm_util.c
  src/dsp/dectalk/src/cmd_init.c
  src/dsp/dectalk/src/epsonapi.c
  src/dsp/dectalk/src/loaddict.c
  src/dsp/dectalk/src/ls_chari.c
  src/dsp/dectalk/src/ls_dict.c
  src/dsp/dectalk/src/ls_homo.c
  src/dsp/dectalk/src/ls_math.c
  src/dsp/dectalk/src/ls_proc.c
  src/dsp/dectalk/src/ls_spel.c
  src/dsp/dectalk/src/ls_speli.c
  src/dsp/dectalk/src/ls_suff.c
  src/dsp/dectalk/src/ls_suffi.c
  src/dsp/dectalk/src/lsa_adju.c
  src/dsp/dectalk/src/lsa_coni.c
  src/dsp/dectalk/src/lsa_rtbi.c
  src/dsp/dectalk/src/lsa_rule.c
  src/dsp/dectalk/src/lsa_task.c
  src/dsp/dectalk/src/lsa_us.c
  src/dsp/dectalk/src/lsa_util.c
  src/dsp/dectalk/src/lsw_main.c
  src/dsp/dectalk/src/maindict.c
  src/dsp/dectalk/src/par_char.c
  src/dsp/dectalk/src/par_dict.c
  src/dsp/dectalk/src/par_pars.c
  src/dsp/dectalk/src/par_rule.c
  src/dsp/dectalk/src/ph_aloph.c
  src/dsp/dectalk/src/ph_claus.c
  src/dsp/dectalk/src/ph_draw.c
  src/dsp/dectalk/src/ph_drwt0.c
  src/dsp/dectalk/src/ph_inton.c
  src/dsp/dectalk/src/ph_main.c
  src/dsp/dectalk/src/ph_romi.c
  src/dsp/dectalk/src/ph_setar.c
  src/dsp/dectalk/src/ph_sort.c
  src/dsp/dectalk/src/ph_syl.c
  src/dsp/dectalk/src/ph_task.c
  src/dsp/dectalk/src/ph_timng.c
  src/dsp/dectalk/src/ph_vdefi.c
  src/dsp/dectalk/src/ph_vset.c
  src/dsp/dectalk/src/phlog.c
  src/dsp/dectalk/src/phprint.c
  src/dsp/dectalk/src/playtone.c
  src/dsp/dectalk/src/services.c
  src/dsp/dectalk/src/spc.c
  src/dsp/dectalk/src/usa_init.c
  src/dsp/dectalk/src/vtm.c
  src/dsp/dectalk/src/vtmiont.c
)
add_library(orm_dectalk_core OBJECT ${_nr_dectalk_srcs})
target_include_directories(orm_dectalk_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/dectalk/include)
target_compile_definitions(orm_dectalk_core PRIVATE
  _REENTRANT NOMME LTSSIM TTSSIM ANSI BLD_DECTALK_DLL ENGLISH ENGLISH_US
  ACCESS32 TYPING_MODE ACNA DISABLE_AUDIO SINGLE_THREADED LIKE_43_OR_44
  NO_FILESYSTEM NO_CHARSET)

# 各格式目标吸收引擎目标文件 (DectalkEngine.cpp 已在 SOURCES 中, 经相对路径
# 包含 dectalk 头文件, 不向插件目标注入 dectalk/include — 其中的 config.h
# 会遮蔽 iPlug2 依赖的插件 config.h)
foreach(_nr_dt_tgt ${PROJECT_NAME}-app ${PROJECT_NAME}-au ${PROJECT_NAME}-vst3)
  if(TARGET ${_nr_dt_tgt})
    target_sources(${_nr_dt_tgt} PRIVATE $<TARGET_OBJECTS:orm_dectalk_core>)
  endif()
endforeach()
