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

inline IColor WarmGray(int v) {
  int vv = ThemeMode() ? 255 - v : v;
  if (ThemeMode())
    vv = std::min(255, vv + 16);
  const float b = vv / 255.f;
  const int B = (int)std::lround(b * 100.f);
  const float s = SatForB(B) / 100.f;
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

} // namespace igraphics
} // namespace iplug
