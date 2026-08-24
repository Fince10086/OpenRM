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
  kTxtRelease,
  kTxtRange,
  kTxtAttack,
  kTxtRes,
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
  kTxtTipRelease,
  kTxtTipRange,
  kTxtTipAttack,
  kTxtTipRes,
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
          "RELEASE",
          "RANGE",
          "ATTACK",
          "RES",
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
          "Spectrum display release time",
          "Spectrum display bottom (dBFS)",
          "Spectrum display attack time",
          "Spectrum analysis FFT size (1024/2048/4096)",
          "AUDIO",
          "Input",
          "Output",
          "Driver",
      },
      {
          "混合",
          "释放",
          "范围",
          "起音",
          "分辨率",
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
          "频谱显示释放时间",
          "频谱显示下限（dBFS）",
          "频谱显示上升时间",
          "频谱分析 FFT 尺寸（1024/2048/4096）",
          "音频",
          "输入",
          "输出",
          "驱动",
      },
  };
  return kTable[lang][id];
}

} // namespace orm
