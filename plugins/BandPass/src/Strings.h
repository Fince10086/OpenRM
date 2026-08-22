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
  kTxtPreset,
  kTxtTipPad,
  kTxtTipBand,
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
      "GAIN L", "GAIN R", "CENTER", "BANDWIDTH", "LOWCUT", "HIGHCUT",
      "LEFT", "RIGHT", "ON", "OFF", "LINK", "FLIP", "L->R", "R->L",
      "UNDO", "REDO", "SAVE", "LOAD",
      "Language", "Dark", "Light", "Theme Color",
      "Preset %d",
      "Drag to set center frequency and bandwidth",
      "Drag the handles to set the low and high cut",
      "Channel gain",
      "Fade time when switching presets",
      "Random amplitude range",
      "Random speed (period)",
      "Dry/wet mix",
      "Preset fade position",
      "Preset slots",
      "Random modulation",
      "Drag onto another slot to swap",
      "⌘Click: save here",
      "Right-click: menu",
    },
    {
      "预设", "随机", "渐变", "范围", "速度", "混合",
      "增益 L", "增益 R", "中心", "带宽", "低切", "高切",
      "左", "右", "开", "关", "联动", "翻转", "左→右", "右→左",
      "撤销", "重做", "保存", "读取",
      "语言", "深色", "浅色", "主题色",
      "预设 %d",
      "拖动设置中心频率与带宽",
      "拖动手柄设置低切与高切",
      "声道增益",
      "切换预设时的渐变时间",
      "随机幅度范围",
      "随机速度（周期）",
      "干湿混合",
      "预设渐变位置",
      "预设槽",
      "随机调制",
      "拖到其他槽位以交换",
      "⌘点击：保存到此槽",
      "右键：菜单",
    },
  };
  return kTable[lang][id];
}

} // namespace orm
