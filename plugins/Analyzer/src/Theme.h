#pragma once

#include "IGraphics.h"
#include "Strings.h"

#include <algorithm>
#include <cmath>

namespace iplug {
namespace igraphics {

constexpr const char *kFontRegular = "Mixed";
constexpr const char *kFontSemiBold = "Mixed-SemiBold";
constexpr const char *kFontBold = "Mixed-Bold";
constexpr const char *kFontSystem = "System";

inline int &ThemeMode() {
  static int mode = 0;
  return mode;
}
inline int &ThemeHue() {
  static int hue = 45;
  return hue;
}
inline int &ThemeSatMax() {
  static int satMax = 15;
  return satMax;
}

constexpr int kLightB[5] = {10, 40, 60, 90, 95};
constexpr int kDarkB[5] = {90, 60, 40, 20, 10};

inline float SatForB(int B) {
  return (float)ThemeSatMax() * std::pow((100.f - (float)B) / 100.f, 1.0f);
}

inline IColor HSBToIColor(int h, float s, float b) {
  const float c = b * s;
  const float hh = h / 60.f;
  const float x = c * (1.f - std::fabs(std::fmod(hh, 2.f) - 1.f));
  float r = 0.f, g = 0.f, bl = 0.f;
  if (hh < 1.f) {
    r = c;
    g = x;
  } else if (hh < 2.f) {
    r = x;
    g = c;
  } else if (hh < 3.f) {
    g = c;
    bl = x;
  } else if (hh < 4.f) {
    g = x;
    bl = c;
  } else if (hh < 5.f) {
    r = x;
    bl = c;
  } else {
    r = c;
    bl = x;
  }
  const float m = b - c;
  return IColor(255, (int)std::lround((r + m) * 255.f), (int)std::lround((g + m) * 255.f),
                (int)std::lround((bl + m) * 255.f));
}

inline IColor COL_900() {
  const int B = ThemeMode() ? kDarkB[0] : kLightB[0];
  return HSBToIColor(ThemeHue(), SatForB(B) / 100.f, B / 100.f);
}
inline IColor COL_700() {
  const int B = ThemeMode() ? kDarkB[1] : kLightB[1];
  return HSBToIColor(ThemeHue(), SatForB(B) / 100.f, B / 100.f);
}
inline IColor COL_500() {
  const int B = ThemeMode() ? kDarkB[2] : kLightB[2];
  return HSBToIColor(ThemeHue(), SatForB(B) / 100.f, B / 100.f);
}
inline IColor COL_300() {
  const int B = ThemeMode() ? kDarkB[3] : kLightB[3];
  return HSBToIColor(ThemeHue(), SatForB(B) / 100.f, B / 100.f);
}
inline IColor COL_100() {
  const int B = ThemeMode() ? kDarkB[4] : kLightB[4];
  return HSBToIColor(ThemeHue(), SatForB(B) / 100.f, B / 100.f);
}

// 按钮 hover 叠层：浅色主题叠黑、深色主题叠白，alpha 混合
inline IColor HoverOverlay() {
  return ThemeMode() ? IColor(48, 255, 255, 255) : IColor(48, 0, 0, 0);
}

// 频谱背景格子灰。深色模式把亮度压缩到近黑窄带、饱和度减半，让频谱亮墨拉开对比；
// 斜率 0.40 决定格子间灰度阶梯，觉得分界弱可调大、整体亮可调低基值 15
inline IColor WarmGray(int v) {
  float vv = (float)v;
  float sScale = 1.f;
  if (ThemeMode()) {
    vv = 15.f + (255.f - (float)v) * 0.40f;
    sScale = 0.5f;
  }
  const float b = vv / 255.f;
  const int B = (int)std::lround(b * 100.f);
  const float s = SatForB(B) * sScale / 100.f;
  return HSBToIColor(ThemeHue(), s, b);
}

static constexpr float HANDLE_R = 7.f;
static constexpr float HANDLE_RING = 1.5f;
static constexpr float LABEL_VALUE_GAP = 12.f;

constexpr float AG_SWATCH = 14.f;
constexpr float AG_SWATCH_GAP = 8.f;

constexpr int kNumRandomColors = 4;

inline IColor RandomColor(int idx) {
  static const IColor kColors[kNumRandomColors] = {
      IColor(255, 224, 66, 61),
      IColor(255, 240, 184, 40),
      IColor(255, 72, 138, 255),
      IColor(255, 80, 190, 96),
  };
  return kColors[std::clamp(idx, 0, kNumRandomColors - 1)];
}

inline IColor RandomColorDim(int idx) {
  const IColor c = RandomColor(idx);
  return IColor(90, c.R, c.G, c.B);
}

inline IColor RandomColorGhost(int idx) {
  const IColor c = RandomColor(idx);
  return IColor(150, c.R, c.G, c.B);
}

enum EPadCorner : int { kCornerCenter = 4, kCornerBw = 5, kCornerLow = 6, kCornerHigh = 7 };

constexpr int kSlopeDb[4] = {12, 24, 48, 96};
constexpr int kSlopeDefaultIdx = 3;

inline IVStyle MakeORMStyle() {
  IVColorSpec colors = {COL_100(), COL_100(), COL_900(), COL_900(), COL_500(),
                        COL_300(), COL_300(), COL_900(), COL_900()};
  const IText labelText(20, COL_700(), kFontRegular, EAlign::Center, EVAlign::Bottom);
  const IText valueText(20, COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Top);
  return IVStyle(true, true, colors, labelText, valueText, true, true, false, false, 0.2f, 1.5f, 0.f, 1.f, 0.f);
}

inline IVStyle MakeButtonStyle() {
  IVColorSpec colors = {COL_100(), COL_100(), COL_900(), COL_900(), COL_500(),
                        COL_300(), COL_300(), COL_900(), COL_900()};
  const IText labelText(20, COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Middle);
  const IText valueText(20, COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Middle);
  return IVStyle(true, true, colors, labelText, valueText, true, true, false, false, 0.f, 2.f, 0.f, 1.f, 0.f);
}

// 频谱三通道颜色：M 用主题色相；L/R 用 ±30° 对称偏移，饱和度 ×1.155 补偿矢量混合微损。
// 配合 SpectrumPad 顶层 alpha 降阶映射，使两层在背景上的有效贡献权重恒等，消除色相漂移
inline void GetChannelColors(IColor &cL, IColor &cR, IColor &cM) {
  auto wrap = [](int h) {
    h %= 360;
    return h < 0 ? h + 360 : h;
  };
  float b, sM, sLR;
  if (ThemeMode()) {
    // 深色模式亮墨：亮度 70%，饱和度随主题放大（×2.5，上限 45%），与压暗背景配对
    b = 0.70f;
    sM = sLR = std::min((float)ThemeSatMax() * 2.5f, 45.f) / 100.f;
  } else {
    b = kLightB[2] / 100.f;
    sM = std::max(ThemeSatMax(), 0) / 100.f;
    sLR = std::max(ThemeSatMax(), 15) / 100.f;
  }
  const float sMix = std::min(sLR * 1.155f, 1.f);
  cL = HSBToIColor(wrap(ThemeHue() - 30), sMix, b);
  cR = HSBToIColor(wrap(ThemeHue() + 30), sMix, b);
  cM = HSBToIColor(ThemeHue(), sM, b);
}

// 电平表语义色：黄/红/过载 LED 用固定安全色（专业表惯例，不随主题旋转）；
// 绿段（≤-18 dB）用通道色，见 SpectrumPad::MeterColorFor
inline IColor MeterYellow() { return IColor(255, 232, 173, 40); }
inline IColor MeterRed() { return IColor(255, 226, 60, 52); }
// 响度达标绿（|Δ|≤1 LU）
inline IColor MeterGreen() { return IColor(255, 96, 186, 96); }
inline IColor MeterOverLed() {
  if (ThemeSatMax() == 0)
    return ThemeMode() ? IColor(255, 250, 250, 250) : IColor(255, 20, 20, 20);
  const float satScale = (ThemeSatMax() <= 30)
                             ? ((float)ThemeSatMax() / 30.f)
                             : (1.f + (float)(ThemeSatMax() - 30) / 55.f);
  const float s = std::clamp(0.85f * satScale, 0.f, 1.f);
  return HSBToIColor(0, s, 0.95f);
}

} // namespace igraphics
} // namespace iplug
