#define PLUG_NAME "ORM BandPass"
#define PLUG_MFR "OpenRM"
#define PLUG_VERSION_HEX 0x00000400
#define PLUG_VERSION_STR "0.5.1"
#define PLUG_UNIQUE_ID 'ORMB'
#define PLUG_MFR_ID 'OpRM'
#define PLUG_URL_STR "https://github.com/OpenRM"
#define PLUG_EMAIL_STR "fince@foxmail.com"
#define PLUG_COPYRIGHT_STR "Copyright 2026 OpenRM"
#define PLUG_CLASS_NAME ORMBandPass

#define BUNDLE_NAME "ORMBandPass"
#define BUNDLE_MFR "OpenRM"
#define BUNDLE_DOMAIN "com"

#define SHARED_RESOURCES_SUBPATH "ORMBandPass"

#define PLUG_CHANNEL_IO "1-1 2-2"

#define PLUG_LATENCY 0
#define PLUG_TYPE 0
#define PLUG_DOES_MIDI_IN 0
#define PLUG_DOES_MIDI_OUT 0
#define PLUG_DOES_MPE 0
#define PLUG_DOES_STATE_CHUNKS 0
#define PLUG_HAS_UI 1
#define PLUG_WIDTH 916
#define PLUG_HEIGHT 646
#define PLUG_FPS 60
#define PLUG_SHARED_RESOURCES 0
#define PLUG_HOST_RESIZE 1

#define AUV2_ENTRY ORMBandPass_Entry
#define AUV2_ENTRY_STR "ORMBandPass_Entry"
#define AUV2_FACTORY ORMBandPass_Factory
#define AUV2_VIEW_CLASS ORMBandPass_View
#define AUV2_VIEW_CLASS_STR "ORMBandPass_View"

// AAX / CLAP 目标未构建, 对应宏已移除; 若将来启用参照 iPlug2 示例补回。

#define VST3_SUBCATEGORY "Fx"

#define APP_NUM_CHANNELS 2
#define APP_N_VECTOR_WAIT 0
#define APP_MULT 1
#define APP_COPY_AUV3 0
#define APP_SIGNAL_VECTOR_SIZE 64

#define MIXED_FN "Mixed-Regular.ttf"
#define MIXED_SB_FN "Mixed-SemiBold.ttf"
#define MIXED_BD_FN "Mixed-Bold.ttf"
