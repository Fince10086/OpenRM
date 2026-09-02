#define PLUG_NAME "ORM Narrator"  // 显示名 (DAW 列表 / app 标题), 允许空格
#define PLUG_MFR "OpenRM"
#define PLUG_VERSION_HEX 0x00000100
#define PLUG_VERSION_STR "0.1.0"
#define PLUG_UNIQUE_ID 'ORMN'
#define PLUG_MFR_ID 'OpRM'
#define PLUG_URL_STR "https://github.com/OpenRM"
#define PLUG_EMAIL_STR "fince@foxmail.com"
#define PLUG_COPYRIGHT_STR "Copyright 2026 OpenRM"
#define PLUG_CLASS_NAME ORMNarrator

#define BUNDLE_NAME "ORMNarrator"
#define BUNDLE_MFR "OpenRM"
#define BUNDLE_DOMAIN "com"

#define SHARED_RESOURCES_SUBPATH "ORMNarrator"

#define PLUG_CHANNEL_IO "0-1 0-2"

#define PLUG_LATENCY 0
// 本项目首个乐器: 接收 MIDI 输入触发语音, 无音频输入
#define PLUG_TYPE 1
#define PLUG_DOES_MIDI_IN 1
#define PLUG_DOES_MIDI_OUT 0
#define PLUG_DOES_MPE 0
#define PLUG_DOES_STATE_CHUNKS 1
#define PLUG_HAS_UI 1
#define PLUG_WIDTH 780
#define PLUG_HEIGHT 720
#define PLUG_FPS 60
#define PLUG_SHARED_RESOURCES 0
#define PLUG_HOST_RESIZE 1

#define AUV2_ENTRY ORMNarrator_Entry
#define AUV2_ENTRY_STR "ORMNarrator_Entry"
#define AUV2_FACTORY ORMNarrator_Factory
#define AUV2_VIEW_CLASS ORMNarrator_View
#define AUV2_VIEW_CLASS_STR "ORMNarrator_View"

// 注: AAX / CLAP 目标未构建, 对应的 AAX_* / CLAP_* 配置已移除 (iPlug2 仅在对应
// 格式编译时才要求这些宏, CLAP 有 #ifndef 兜底)。若将来启用, 参照 iPlug2 示例补回。

// 乐器类目 (VST3 plist 由 iPlug2 依此宏生成分类)
#define VST3_SUBCATEGORY "Instrument"

#define APP_NUM_CHANNELS 2
#define APP_N_VECTOR_WAIT 0
#define APP_MULT 1
#define APP_COPY_AUV3 0
#define APP_SIGNAL_VECTOR_SIZE 64

#define MIXED_FN "Mixed-Regular.ttf"
#define MIXED_SB_FN "Mixed-SemiBold.ttf"
#define MIXED_BD_FN "Mixed-Bold.ttf"
