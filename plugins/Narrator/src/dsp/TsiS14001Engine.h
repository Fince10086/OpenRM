// TsiS14001Engine.h — TSI/SSi S14001A (1975, "Custom ROM Controller") 引擎
//
// 与 TMS5220/SAM 不同, S14001A 不是语音合成芯片, 而是增量波形播放器:
// ROM 存 2-bit delta 流, 芯片按词选择总线 (6 位, 每词表上限 64 词) 播放,
// 输出 4-bit 电平 (0..15, 中心 7)。音高完全由外部时钟决定 (基频 = clock/128)。
// 状态机移植自 MAME src/devices/sound/s14001a.cpp (BSD-3-Clause);
// 词库数据见 dsp/s14001/S14001Roms.h。
//
// 渲染模型: 文本 = 词索引 (空格分隔, 如 "W03 12"), 每词一个完整播放周期;
// 未收录 (索引越界/非法 token/跑飞) 的词自动跳过。
#pragma once

#include "s14001/S14001Roms.h"

#include <string>
#include <vector>

namespace orm
{

// TSI 专属音色参数 (与 SAM/TMS 解耦, 各引擎独立存取)
struct TsiSettings
{
  int speed = 72; // 1..255, 时钟缩放轴 (speed/72 = 倍数, 1.0 = 原生芯片时钟;
  //                越大时钟越快: 音高越高、语速越快)
  int bank = 0;   // 子集索引 (0=BZK 1=F2K 2..8=CSC0..CSC6, 见 s14001::kSets)
};

class TsiS14001Engine
{
public:
  // text: 一个或多个词索引, 空格分隔 (大写/小写均可, "W03"/"3"/"w12"); 0..63.
  // set: 子集索引 (见 s14001::kSets)。
  // rateScale: 时钟缩放, 1.0 = 原生时钟 (0.25..4.0 之外钳制)。
  // out: 单声道 float (采样率 = RateForSet), 峰值归一到 0.85。
  //      无任何可渲染词返回 false。
  static bool Render(const std::string& text, int set, float rateScale,
                     std::vector<float>& out);

  // 渲染采样率 = 原生时钟 * rateScale (VoiceRenderer 据此重采样到宿主率)
  static double RateForSet(int set, float rateScale);
};

} // namespace orm