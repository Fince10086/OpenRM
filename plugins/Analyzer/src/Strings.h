#pragma once

namespace orm {

enum ELanguage { kLangEN = 0, kLangZH, kNumLanguages };
int DetectSystemLanguage();

inline int &UILang() {
  static int lang = DetectSystemLanguage();
  return lang;
}

enum EText {
  kTxtMix,
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
  kTxtTipMix,
  kTxtAudio,
  kTxtAudioInput,
  kTxtAudioOutput,
  kTxtDriver,
  kNumTexts
};

inline const char *Tr(int id, int lang) {
  static const char *const kTable[kNumLanguages][kNumTexts] = {
      {
          "MIX",
          "UNDO",
          "REDO",
          "SAVE",
          "LOAD",
          "LANGUAGE",
          "DARK",
          "LIGHT",
          "Theme Color",
          "SETTINGS",
          "THEME",
          "Hue",
          "Saturation",
          "NONE",
          "LOW",
          "MED",
          "HIGH",
          "中文",
          "Dry/wet mix",
          "AUDIO",
          "Input",
          "Output",
          "Driver",
      },
      {
          "混合",
          "撤销",
          "重做",
          "保存",
          "读取",
          "语言",
          "深色",
          "浅色",
          "主题色",
          "设置",
          "主题",
          "色相",
          "饱和度",
          "无",
          "低",
          "中",
          "高",
          "中文",
          "干湿混合",
          "音频",
          "输入",
          "输出",
          "驱动",
      },
  };
  return kTable[lang][id];
}

} // namespace orm
