// Tms5220Engine.h — TMS5220 (1980, Texas Instruments Speak & Spell) 引擎
//
// LPC-10 位流合成核。算法与量化表取自 Talkie library (Peter Knight, GPLv2+;
// Armin Joachimsmeyer 复刻版, GPLv3+), 本插件以 GPL 分发, 许可兼容。
// 词汇数据: TI VM61002/3/4/5 官方词表 (见 dsp/tms/TmsVocab.h) 及其余音色词表
//   (dsp/tms/VocabTI99.h / VocabAcorn.h / VocabClock.h)。
// 渲染模型: 词名 → LPC 位流 → 逐帧解码 → 格型合成 → 单声道 float, 8 kHz。
#pragma once

#include <string>
#include <vector>

namespace orm
{

// TMS 专属音色参数 (与 SAM 解耦, 各引擎独立存取)
struct TmsSettings
{
  int speed = 72; // 1..255, 帧时长缩放轴 (speed/72 = 速率倍数, 1.0 原速)
  int pitch = 64; // 0..255, PHRASE 模式附加变调量 (与 SAM pitch 无关)
  int bank = 0;   // 音色/词库: 0=Military 1=TI-99 2=Acorn 3=Clock
};

class Tms5220Engine
{
public:
  static constexpr double kSampleRate = 8000.0;

  // text: 一个或多个词库名词, 空格分隔 (大写, 如 "DANGER ONE TWO" 或 "HELLO DANGER");
  //       当前所选音色词库未收录的词跳过。串接方式与 Talkie 的词 FIFO 一致: 停止帧后接
  //       下一个词, 格型滤波器状态跨词持续, 系数在停止帧清零 (词间自然衰减衔接)。
  // bank: 音色/词库索引 (见 TmsSettings::bank)。
  // speedScale: 帧时长缩放, 1.0 = 原速, 2.0 = 慢一倍 (0.25..4.0 之外钳制)。
  // out: 单声道 float, 峰值归一到 0.85。无任何可渲染词返回 false。
  static bool Render(const std::string& text, int bank, float speedScale, std::vector<float>& out);
};

} // namespace orm

