// Sp0256Engine.h — GI/Microchip SP0256-AL2 "Narrator" (1981) 引擎
//
// 与 TMS5220/TMS5110/SAM 不同, SP0256 不是整词播放器, 而是 allophone (音素)
// 合成器: 芯片内置微序控制器从 ROM 位流解码 13 种参数指令 (LOAD/DELTA/SETMSB/
// PAUSE...), 驱动 12 阶 (6×2 阶节) 格型滤波器。算法与参数表移植自 MAME
// src/devices/sound/sp0256.cpp (BSD-3-Clause); 语音 ROM 数据见
// dsp/sp0256/Sp0256Roms.h (GmEsoft 逆向提取, GPL-3.0-or-later)。
//
// 渲染模型: 两种输入模式 (kSp0256Voice / Sp0256Settings.variant):
//   音素模式 (kPhonemeVariant): 标签输入, 空格分隔 — 合并 AL2 allophone 标签
//     (PA1..PA5 + 59 音素) 与 012 Intellivoice 单词标签 (Zero/One/...), 另有少量
//     预设英语短语 (HELLO/THANK/... 自动展开为 allophone 串)。**不接受数字码**。
//   文本模式 (kTextVariant): 任意英文文本, 经 CTS256A-AL2 控制器转 allophone 码。
// 采样率 = XTAL/312 (默认 3.12MHz = 10 kHz); 语速参数缩放 XTAL: 采样数不变,
// 时钟越快则时长越短、音高越高。
#pragma once

#include "Cts256aEngine.h"
#include "sp0256/Sp0256Roms.h"

#include <string>
#include <vector>

namespace orm
{

// SP0256 专属音色参数 (与 SAM/TMS/TSI 解耦, 各引擎独立存取)
struct Sp0256Settings
{
  int speed = 72;    // 1..255, XTAL 缩放轴 (speed/72 = 倍数, 1.0 = 3.12MHz;
  //                越大时钟越快: 音高越高、语速越快, 采样数不变)
  int variant = 0;   // 输入模式: 0=文本 (TTS) 1=音素 (AL2+012 合并标签)
};

class Sp0256Engine
{
public:
  static constexpr double kDefaultClock = 3120000.0; // 默认 XTAL 频率
  static constexpr double kSampleRate = 10000.0;     // 默认 XTAL/312
  // 文本模式: 输入为英文文本, 经 CTS256A-AL2 控制器 (TMS7000 + 4KB 规则引擎)
  // 转成 allophone 码后用 AL2 ROM 渲染 (见 Cts256aEngine)。
  static constexpr int kTextVariant = 0;    // 文本模式 (CTS256A 英文字符转写)
  static constexpr int kPhonemeVariant = 1; // 音素模式 (AL2 音素 + 012 单词标签合并)
  // 语音 ROM 数据索引 (sp0256::kVariants): 音素模式按标签来源切换 ROM
  static constexpr int kAl2Rom = 0;       // Narrator allophone ROM
  static constexpr int kIntellivRom = 1;  // Intellivoice 单词 ROM

  // text: 空格分隔的输入, 大小写不敏感。
  //   文本模式 (variant 0): 任意英文文本 (字母/数字/标点, CTS256A 规则转写);
  //   音素模式 (variant 1): AL2 allophone 标签 (PA1..PA5 + 59 音素) / 012 单词
  //     标签 (Zero One ...) / 预设英语短语; **数字码不接受** (整段数字视为未收录)。
  //   未收录内容跳过; 全无效返回 false。
  // speedScale: XTAL 缩放, 1.0 = 3.12MHz (0.25..4.0 之外钳制)。
  // out: 单声道 float (采样率 = RateFor), 峰值归一到 0.85。
  static bool Render(const std::string& text, int variant, float speedScale,
                     std::vector<float>& out);

  // XTAL 频率 = 3.12MHz * speedScale (VoiceRenderer 计算时长用)
  static double ClockFor(float speedScale);
  // 渲染采样率 = XTAL/312 (VoiceRenderer 据此重采样到宿主率)
  static double RateFor(float speedScale);
};

} // namespace orm