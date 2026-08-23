#pragma once

#include "IGraphics.h"
#include "Strings.h"

#include <cmath>

namespace iplug { namespace igraphics {

inline const char* FontRegular()  { return orm::UILang() == orm::kLangZH ? "Mixed" : "Outfit"; }
inline const char* FontSemiBold() { return orm::UILang() == orm::kLangZH ? "Mixed-SemiBold" : "Outfit-SemiBold"; }
inline const char* FontBold()     { return orm::UILang() == orm::kLangZH ? "Mixed-Bold" : "Outfit-Bold"; }

inline const char* FontCJKRegular()  { return "Mixed"; }
inline const char* FontCJKSemiBold() { return "Mixed-SemiBold"; }
inline const char* FontCJKBold()     { return "Mixed-Bold"; }

inline int& ThemeMode()   { static int mode = 0; return mode; }
inline int& ThemeHue()    { static int hue = 45; return hue; }
inline int& ThemeSatMax() { static int satMax = 15; return satMax; }

constexpr int kLightB[5] = { 10, 40, 60, 90, 95 };
constexpr int kDarkB[5]  = { 90, 60, 40, 20, 10 };

// Saturation = satMax * ((100 - B) / 100)^1.0
inline float SatForB(int B)
{
  return (float) ThemeSatMax() * std::pow((100.f - (float) B) / 100.f, 1.0f);
}

inline IColor HSBToIColor(int h, float s, float b)
{
  const float c = b * s;
  const float hh = h / 60.f;
  const float x = c * (1.f - std::fabs(std::fmod(hh, 2.f) - 1.f));
  float r = 0.f, g = 0.f, bl = 0.f;
  if (hh < 1.f)      { r = c; g = x; }
  else if (hh < 2.f) { r = x; g = c; }
  else if (hh < 3.f) { g = c; bl = x; }
  else if (hh < 4.f) { g = x; bl = c; }
  else if (hh < 5.f) { r = x; bl = c; }
  else               { r = c; bl = x; }
  const float m = b - c;
  return IColor(255, (int) std::lround((r + m) * 255.f),
                     (int) std::lround((g + m) * 255.f),
                     (int) std::lround((bl + m) * 255.f));
}

inline IColor COL_900() { const int B = ThemeMode() ? kDarkB[0] : kLightB[0]; return HSBToIColor(ThemeHue(), SatForB(B) / 100.f, B / 100.f); }
inline IColor COL_700() { const int B = ThemeMode() ? kDarkB[1] : kLightB[1]; return HSBToIColor(ThemeHue(), SatForB(B) / 100.f, B / 100.f); }
inline IColor COL_500() { const int B = ThemeMode() ? kDarkB[2] : kLightB[2]; return HSBToIColor(ThemeHue(), SatForB(B) / 100.f, B / 100.f); }
inline IColor COL_300() { const int B = ThemeMode() ? kDarkB[3] : kLightB[3]; return HSBToIColor(ThemeHue(), SatForB(B) / 100.f, B / 100.f); }
inline IColor COL_100() { const int B = ThemeMode() ? kDarkB[4] : kLightB[4]; return HSBToIColor(ThemeHue(), SatForB(B) / 100.f, B / 100.f); }

inline IColor WarmGray(int v)
{
  int vv = ThemeMode() ? 255 - v : v;
  if (ThemeMode()) vv = std::min(255, vv + 16);    // lift dark-mode ramp ~6%
  const float b = vv / 255.f;                       // brightness 0..1
  const int B = (int) std::lround(b * 100.f);       // brightness 0..100 for the formula
  const float s = SatForB(B) / 100.f;               // saturation 0..1
  return HSBToIColor(ThemeHue(), s, b);
}

static constexpr float BLOCK_GAP = 1.5f;
static constexpr float HANDLE_R = 7.f;
static constexpr float HANDLE_RING = 1.5f;
static constexpr float LABEL_VALUE_GAP = 12.f;

enum EPadCorner : int
{
  kCornerCenter = 4,
  kCornerBw     = 5,
  kCornerLow    = 6,
  kCornerHigh   = 7
};

constexpr int kSlopeDb[4] = { 12, 24, 48, 96 };
constexpr int kSlopeDefaultIdx = 3; // 96 dB/oct

} }
