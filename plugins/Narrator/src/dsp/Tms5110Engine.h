// Tms5110Engine.h — TMS5110 (TMC0281, 1978 Speak & Spell) 引擎
//
// 与 TMS5220 同属 TI LPC 家族: 帧结构相同 (energy/repeat/pitch/k1..k10), 但
// pitch 字段为 5 位 (5220 为 6 位), 且使用 1978 TMC0281 的专利参数表
// (energty/pitch/K/chirp 均与 5220 不同)。算法/参数表参考 MAME tms5110.cpp
// (BSD-3-Clause) 与 tms5110r.hxx; 词表见 dsp/tms/VocabSspell.h。
#pragma once

#include <string>
#include <vector>

namespace orm
{

class Tms5110Engine
{
public:
  static constexpr double kSampleRate = 8000.0;

  // text: 一个或多个词 (大写, 空格分隔, 如 "ISLE COLOR"); 词表未收录的词跳过。
  // speedScale: 帧时长缩放, 1.0 = 原速 (0.25..4.0 之外钳制)。
  // out: 单声道 float, 峰值归一到 0.85。无任何可渲染词返回 false。
  static bool Render(const std::string& text, float speedScale, std::vector<float>& out);
};

} // namespace orm