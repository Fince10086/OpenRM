// Sp0256Engine.h — GI/Microchip SP0256-AL2 "Narrator" (1981) 引擎
//
// 与 TMS5220/TMS5110/SAM 不同, SP0256 不是整词播放器, 而是 allophone (音素)
// 合成器: 芯片内置微序控制器从 ROM 位流解码 13 种参数指令 (LOAD/DELTA/SETMSB/
// PAUSE...), 驱动 12 阶 (6×2 阶节) 格型滤波器。算法与参数表移植自 MAME
// src/devices/sound/sp0256.cpp (BSD-3-Clause); 语音 ROM 数据见
// dsp/sp0256/Sp0256Roms.h (GmEsoft 逆向提取, GPL-3.0-or-later)。
//
// 渲染模型: 文本 = 标签或数字码 (空格分隔)。AL2 档接受 allophone 标签
// (PA1..PA5 + 59 音素) 与少量预设英语短语; 012 档接受 Intellivoice 单词标签。
// 采样率 = XTAL/312 (默认 3.12MHz = 10 kHz); 语速参数缩放 XTAL: 采样数不变,
// 时钟越快则时长越短、音高越高。
#pragma once

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
  int variant = 0;   // 语音版本: 0=AL2 (Narrator allophone) 1=012 (Intellivoice 单词)
};

class Sp0256Engine
{
public:
  static constexpr double kDefaultClock = 3120000.0; // 默认 XTAL 频率
  static constexpr double kSampleRate = 10000.0;     // 默认 XTAL/312

  // text: 空格分隔的标签或数字码 (大小写不敏感, 码 0..63)。
  //   AL2 (variant 0): allophone 标签 (PA1..PA5 + 59 音素, 见 sp0256::kAl2Labels),
  //     数字码, 或预设英语短语 (HELLO/THANK/YOU/GOOD/BYE/WORLD/SPEECH/TEST/
  //     ONE/TWO/THREE/ZERO, 见本文件 kPhrases); 其余 token 跳过。
  //   012 (variant 1): Intellivoice 单词标签 (ZERO/ONE/.../AND, 见
  //     sp0256::k012Labels) 或数字码; 其余 token 跳过。
  //   全无效返回 false。
  // variant: 0=AL2 1=012 (见 sp0256::kVariants)。
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