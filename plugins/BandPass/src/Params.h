#pragma once

// 参数枚举与全局常量。
// 独立成头的原因: UI 控件 (controls/*) 与插件本体 (BandPass.h) 都要引用 EParams,
// 若留在 BandPass.h 会导致控件头反向依赖插件头; 这里只依赖 <array>, 双方安全包含。

#include <array>

enum EParams {
  kFreqL = 0,
  kBwL,
  kGainL,
  kFreqR,
  kBwR,
  kGainR,
  kLink,
  kMix,
  kRandomAmountR,
  kRandomRateR,
  kSlopeL,
  kSlopeR,
  kRandomColorFreqL,
  kRandomColorBwL,
  kRandomColorGainL,
  kRandomColorFreqR,
  kRandomColorBwR,
  kRandomColorGainR,
  kRandomColorMix,
  kRandomEnableFreqL,
  kRandomEnableBwL,
  kRandomEnableGainL,
  kRandomEnableFreqR,
  kRandomEnableBwR,
  kRandomEnableGainR,
  kRandomEnableMix,
  kRandomAmountY,
  kRandomRateY,
  kRandomAmountB,
  kRandomRateB,
  kRandomAmountG,
  kRandomRateG,
  kPassL,
  kPassR,
  kNumParams
};

using ParamSnapshot = std::array<double, kNumParams>;

constexpr int kNumPresets = 24;
constexpr int kNumQuick = 8; // 底部快速槽数量（= 渐变滑杆刻度数）

constexpr int kSpectrumFFTSize = 4096;
constexpr int kSpectrumOverlap = 4;

enum EControlTags { kCtrlTagPadL = 100, kCtrlTagPadR = 101 };
