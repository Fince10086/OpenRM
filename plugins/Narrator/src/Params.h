#pragma once

// 参数枚举独立成头: 插件本体 (Narrator.h/.cpp) 与 UI 控件 (ORMSlider 等)
// 都要引用参数索引, 避免控件反向依赖插件头文件。

#include <array>

enum EEngine
{
  kEngineSAM = 0,
  kEngineDEC = 1,
  kEngineSP = 2,
  kEngineTMS = 3,
  kEngineTSI = 4,
  kNumEngines = 5
};

enum EParams
{
  kEngine = 0,  // 合成引擎: 0=SAM 1=DEC 2=SP 3=TMS 4=TSI (见 EEngine)
  kMapMode,     // 映射模式: 0=PITCH 音高映射 1=WORDS 词语映射
  kBaseKey,     // 触发基准键 (PITCH 模式音高偏移的 0 点)
  // ---- SAM 专属音色参数 (与 TMS 解耦, 各引擎独立存取) ----
  kSamPitch,    // SAM 原生音高 (0..255)
  kSamSpeed,    // SAM 原生语速 (值越大越慢)
  kSamMouth,    // SAM 口腔形变
  kSamThroat,   // SAM 喉腔形变
  // ---- TMS 专属 (与 SAM 解耦) ----
  kTmsSpeed,    // TMS 帧时长缩放 (值/72 = 速率倍数, 1.0 原速)
  kTmsPitch,    // TMS PHRASE 模式附加变调量 (varispeed)
  kTmsBank,     // TMS 音色/词库 (见 kBank* 常量)
  // ---- TSI S14001A 专属 (与 SAM/TMS 解耦) ----
  kTsiSpeed,    // TSI 芯片时钟缩放 (值/72 = 倍数, 1.0 原生时钟; 越大越快越高)
  kTsiBank,     // TSI 子集 (见 kS14001* 常量)
  // ---- SP0256 专属 (与 SAM/TMS/TSI 解耦) ----
  kSp0256Speed, // SP0256 XTAL 时钟缩放 (值/72 = 倍数, 1.0 = 3.12MHz; 越大越快越高)
  kSp0256Voice, // SP0256 语音版本 (见 kSp0256* 常量)
  // ---- DECTALK 专属 (与 SAM/TMS/TSI/SP0256 解耦) ----
  kDectalkVoice, // DECtalk 音色 (见 orm::kDectalk* 常量, 0=Paul)
  kDectalkRate,  // DECtalk 说话速率 (75..600, 引擎原生 ≈ 180)
  kDectalkPitch, // DECtalk 平均音高 AP (Hz, 0 = 音色原生)
  // ---- 通用 (各引擎共用) ----
  kAttack,      // 触发起音 ms
  kRelease,     // 松键释放 ms
  kMono,        // 单音 (true) / 复音 (false)
  kGain,        // 输出电平 dB
  kLoop,        // 按键未松开时循环重放其音频
  kNumParams
};

// 全参数快照 (撤销/重做的原子单位, 与 Analyzer 同构)
using ParamSnapshot = std::array<double, kNumParams>;

// TMS 音色/词库索引 (kTmsBank) — 顺序即 UI 音色选择段的顺序
enum
{
  kBankMilitary = 0, // VM61002/3/4/5 官方/军事
  kBankTi99 = 1,     // TI-99/4A 语音模块 (1979)
  kBankAcorn = 2,    // Acorn BBC 语音系统 (1983)
  kBankSspell = 3,   // TMS5110 / TMC0281 — Speak & Spell (1978)
  kBankClock = 4,    // VM61002 衍生女声时钟短语
  kNumBanks = 5
};

// TSI S14001A 子集索引 (kTsiBank) — 顺序即 TSI 音色选择段的顺序
enum
{
  kS14001Bzk = 0,  // Stern Berzerk 街机 (VSU-1000 语音板)
  kS14001F2k = 1,  // Stern Flight 2000 弹球 (MP-200)
  kS14001Csc0 = 2, // Fidelity CSC 语音棋 BIOS0 (101-32107)
  kS14001Csc1 = 3, // 64101 低半区
  kS14001Csc2 = 4, // 64101 高半区
  kS14001Csc3 = 5, // 64105 低半区
  kS14001Csc4 = 6, // 64105 高半区
  kS14001Csc5 = 7, // 64106 低半区
  kS14001Csc6 = 8, // 64106 高半区
  kNumS14001Sets = 9
};

// SP0256 输入模式索引 (kSp0256Voice) — 顺序即 SP0256 选择段的顺序 (与 SAM 控件一致)
enum
{
  kSp0256Text = 0,    // 文本模式: 英文文本经 CTS256A-AL2 规则转写 (原 TTS 档)
  kSp0256Phoneme = 1, // 音素模式: AL2 allophone 标签 + 012 单词标签 (原 AL2/012 合并, 无数字码)
  kNumSp0256 = 2
};

// 跨线程数据绑定用的控件 tag (AttachControl 第二参)
enum EControlTags
{
  kCtrlTagKeyboard = 100, // 屏幕键盘: 宿主 MIDI 回显
  kCtrlTagTimeline,       // 语句时间线: 波形 + 播放指针
};

// 时间线包络点数 (音频线程推送的波形分辨率)
constexpr int kPhraseEnvPoints = 256;

// 宿主块尺寸上限 (超长块在 ProcessBlock 内钳制)
constexpr int kMaxBlock = 16384;