#pragma once

// 参数枚举与全局常量。
// 独立成头的原因: UI 控件 (controls/*) 与插件本体 (Analyzer.h) 都要引用 EParams,
// 若留在 Analyzer.h 会导致控件头反向依赖插件头; 这里只依赖 <array>, 双方安全包含。
//
// Analyzer 是从 BandPass 减出来的第一步: mix 参数 (撤销/重做、保存/读取的载体),
// release/attack 参数 (频谱显示释放/上升时间), range 参数 (频谱显示下限 dBFS 幅度),
// 频谱显示本身为纯展示, 无参数。

#include <array>

enum EParams {
  kMix = 0,
  kRelease,
  kRange,
  kAttack,
  kNumParams
};

using ParamSnapshot = std::array<double, kNumParams>;

constexpr int kSpectrumFFTSize = 4096;
constexpr int kSpectrumOverlap = 4;

enum EControlTags { kCtrlTagPad = 100 };
