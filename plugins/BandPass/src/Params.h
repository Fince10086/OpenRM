#pragma once

// 参数枚举与全局常量 (独立成头避免控件与插件本体循环依赖)

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
constexpr int kNumQuick = 8;

constexpr int kSpectrumFFTSize = 4096;
constexpr int kSpectrumOverlap = 4;

enum EControlTags { kCtrlTagPadL = 100, kCtrlTagPadR = 101 };
