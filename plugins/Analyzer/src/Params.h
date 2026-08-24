#pragma once

// 参数枚举与全局常量。
// 独立成头的原因: UI 控件 (controls/*) 与插件本体 (Analyzer.h) 都要引用 EParams,
// 若留在 Analyzer.h 会导致控件头反向依赖插件头; 这里只依赖 <array>, 双方安全包含。
//
// Analyzer 是从 BandPass 减出来的第一步: 只保留 mix 一个参数
// (供撤销/重做、保存/读取的载体), 频谱显示为纯展示, 无参数。

#include <array>

enum EParams {
  kMix = 0,
  kNumParams
};

using ParamSnapshot = std::array<double, kNumParams>;

constexpr int kSpectrumFFTSize = 4096;
constexpr int kSpectrumOverlap = 4;

enum EControlTags { kCtrlTagPad = 100 };
