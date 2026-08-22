#pragma once

namespace orm {

enum ELanguage { kLangEN = 0, kLangZH, kNumLanguages };

inline int& UILang()
{
  static int lang = kLangEN;
  return lang;
}

enum EText
{
  kTxtPresets,
  kTxtRandom,
  kTxtMorph,
  kTxtRange,
  kTxtSpeed,
  kTxtMix,
  kTxtGainL,
  kTxtGainR,
  kTxtCenter,
  kTxtBandwidth,
  kTxtSlope,
  kTxtLowCut,
  kTxtHighCut,
  kTxtLeft,
  kTxtRight,
  kTxtOn,
  kTxtOff,
  kTxtLink,
  kTxtFlip,
  kTxtCopyLR,
  kTxtCopyRL,
  kTxtUndo,
  kTxtRedo,
  kTxtSave,
  kTxtLoad,
  kTxtLanguage,
  kTxtDark,
  kTxtLight,
  kTxtThemeColor,
  kTxtSettings,
  kTxtTheme,
  kTxtHue,
  kTxtSaturation,
  kTxtSatNone,
  kTxtSatLow,
  kTxtSatMed,
  kTxtSatHigh,
  kTxtChinese,
  kTxtPreset,
  kTxtTipPad,
  kTxtTipBand,
  kTxtTipSlope,
  kTxtTipGain,
  kTxtTipMorph,
  kTxtTipRange,
  kTxtTipSpeed,
  kTxtTipMix,
  kTxtTipFade,
  kTxtTipPresets,
  kTxtTipRandom,
  kTxtTipDrag,
  kTxtTipSaveHere,
  kTxtTipMenu,
  kNumTexts
};

inline const char* Tr(int id, int lang)
{
  static const char* const kTable[kNumLanguages][kNumTexts] =
  {
    {
      "PRESETS", "RANDOM", "MORPH", "RANGE", "SPEED", "MIX",
      "GAIN L", "GAIN R", "CENTER", "BANDWIDTH", "SLOPE", "LOWCUT", "HIGHCUT",
      "LEFT", "RIGHT", "ON", "OFF", "LINK", "FLIP", "L->R", "R->L",
      "UNDO", "REDO", "SAVE", "LOAD",
      "Language", "Dark", "Light", "Theme Color", "Settings",
      "Theme", "Hue", "Saturation", "None", "Low", "Med", "High", "中文",
      "Preset %d",
      "Drag to set center frequency and bandwidth",
      "Drag the handles to set the low and high cut",
      "Band-pass rolloff slope",
      "Channel gain",
      "Fade time when switching presets",
      "Random amplitude range",
      "Random speed (period)",
      "Dry/wet mix",
      "Preset fade position",
      "Preset slots",
      "Random modulation",
      "Drag onto another slot to swap",
      "Cmd+Click: save here",
      "Right-click: menu",
    },
    {
      "预设", "随机", "渐变", "范围", "速度", "混合",
      "增益 L", "增益 R", "中心", "带宽", "滚降", "低切", "高切",
      "LEFT", "RIGHT", "开", "关", "同步", "翻转", "左→右", "右→左",
      "撤销", "重做", "保存", "读取",
      "语言", "深色", "浅色", "主题色", "设置",
      "主题", "色相", "饱和度", "无", "低", "中", "高", "中文",
      "预设 %d",
      "拖动设置中心频率与带宽",
      "拖动手柄设置低切与高切",
      "带通滚降斜率",
      "声道增益",
      "切换预设时的渐变时间",
      "随机幅度范围",
      "随机速度（周期）",
      "干湿混合",
      "预设渐变位置",
      "预设槽",
      "随机调制",
      "拖到其他槽位以交换",
      "Cmd+点击：保存到此槽",
      "右键：菜单",
    },
  };
  return kTable[lang][id];
}

} // namespace orm
