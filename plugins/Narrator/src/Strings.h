#pragma once

namespace orm {

enum ELanguage { kLangEN = 0, kLangZH, kNumLanguages };
int DetectSystemLanguage();

inline int &UILang() {
  static int lang = DetectSystemLanguage();
  return lang;
}

enum EText {
  // ---- 设置面板必需的公共条目 (SettingsPanelControl / UiUtils 引用, 保持稳定) ----
  kTxtLanguage,
  kTxtDark,
  kTxtLight,
  kTxtTheme,
  kTxtHue,
  kTxtSaturation,
  kTxtSatNone,
  kTxtSatLow,
  kTxtSatMed,
  kTxtSatHigh,
  kTxtChinese,
  kTxtAudio,
  kTxtAudioInput,
  kTxtAudioOutput,
  kTxtDriver,
  kTxtRed,
  kTxtYellow,
  kTxtBlue,
  kTxtGreen,
  // ---- Narrator 专有条目 ----
  kTxtMapPitch,      // PITCH / 音高映射
  kTxtMapWords,      // WORDS / 词语映射
  kTxtText,          // 文本标签
  kTxtPhonetic,      // 音素
  kTxtPlay,          // 试听
  kTxtClear,         // 清空
  kTxtPitch,         // Pitch
  kTxtSpeed,         // Speed
  kTxtMouth,         // Mouth
  kTxtThroat,        // Throat
  kTxtAttack,        // Attack
  kTxtRelease,       // Release
  kTxtOutput,        // Output
  kTxtMono,          // MONO
  kTxtPoly,          // POLY
  kTxtLoop,          // LOOP (按住循环)
  kTxtRetrig,        // RETRIG
  kTxtBaseKey,       // Base Key
  kTxtNarratorTitle, // Narrator
  kTxtEngine,        // SAM (引擎名标签, 当前固定)
  kTxtVoice,         // VOICE (TMS 音色选择标签)
  kTxtTsiRate,       // Rate (TSI 时钟/速率)
  kTxtSp0256Rate,    // Rate (SP0256 XTAL/速率)
  kTxtTipKeyboard,
  kTxtTipPhrase,
  kTxtTipTimeline,
  kTxtTipPitch,
  kTxtTipSpeed,
  kTxtTipMouthThroat,
  kTxtTipAttackRelease,
  kTxtTipBaseKey,
  kTxtClickToTalk,   // 点击键盘或按宿主 MIDI 触发
  kTxtEmptyPhrase,   // (空) / (empty)
  kTxtDuration,      // %0.2f s (时长标签)
  kTxtNative,        // native / 原生 (DECTalk 音高 0 = 音色原生)
  kTxtTipDectalkVoice,
  kTxtTipDectalkRate,
  kTxtTipDectalkPitch,
  kNumTexts
};

inline const char *Tr(int id, int lang) {
  static const char *const kTable[kNumLanguages][kNumTexts] = {
      {
          // EN
          "LANGUAGE",
          "DARK",
          "LIGHT",
          "THEME",
          "Hue",
          "Saturation",
          "NONE",
          "LOW",
          "MED",
          "HIGH",
          "中文",
          "AUDIO",
          "Input",
          "Output",
          "Driver",
          "Red",
          "Yellow",
          "Blue",
          "Green",
          "PITCH",
          "WORDS",
          "TEXT",
          "PHONEMES",
          "PLAY",
          "CLEAR",
          "Pitch",
          "Speed",
          "Mouth",
          "Throat",
          "Attack",
          "Release",
          "Output",
          "MONO",
          "POLY",
          "LOOP",
          "RETRIG",
          "Base Key",
          "Narrator",
          "SAM",
          "VOICE",
          "Rate",
          "Rate",
          "Click keys or play host MIDI to trigger speech",
          "Click to edit the phrase text (English)",
          "Rendered phrase waveform and play position",
          "SAM native pitch (0-255)",
          "SAM native speed (higher = slower)",
          "SAM mouth/throat formant shaping",
          "Gate attack/release per note",
          "Reference key: played pitch = note offset from this key",
          "Press PLAY or hit a key to hear it",
          "(empty)",
          "%.2f s",
          "Native",
          "DECtalk voice (np=Paul nb=Betty nh=Harry nf=Frank nd=Dennis nk=Kit nu=Ursula nr=Rita nw=Wendy)",
          "DECtalk speaking rate in words per minute (engine native ~180)",
          "DECtalk average pitch AP in Hz (0 = voice native)",
      },
      {
          // ZH
          "语言",
          "深色",
          "浅色",
          "主题",
          "色相",
          "饱和度",
          "无",
          "低",
          "中",
          "高",
          "中文",
          "音频",
          "输入",
          "输出",
          "驱动",
          "红",
          "黄",
          "蓝",
          "绿",
          "音高映射",
          "词语映射",
          "文本",
          "音素",
          "试听",
          "清空",
          "音高",
          "语速",
          "口腔",
          "喉腔",
          "起音",
          "释放",
          "输出",
          "单音",
          "复音",
          "循环",
          "重触发",
          "基准键",
          "Narrator",
          "SAM",
          "音色",
          "速率",
          "速率",
          "点击键盘或在宿主里弹 MIDI 触发说话",
          "点击编辑语句文本（仅英文）",
          "已渲染语句的波形与播放位置",
          "SAM 原生音高 (0-255)",
          "SAM 原生语速（值越大越慢）",
          "SAM 口腔/喉腔共振峰塑形",
          "每音符的门限起音/释放",
          "基准键：演奏音高相对此键的半音偏移",
          "按试听或任意琴键试听",
          "（空）",
          "%.2f 秒",
          "原生",
          "DECtalk 音色 (np=Paul nb=Betty nh=Harry nf=Frank nd=Dennis nk=Kit nu=Ursula nr=Rita nw=Wendy)",
          "DECtalk 说话速率 (词/分钟, 引擎原生 ≈ 180)",
          "DECtalk 平均音高 AP (Hz, 0 = 音色原生)",
      },
  };
  return kTable[lang][id];
}

} // namespace orm
