#pragma once

#include "IGraphics.h"
#include "Strings.h"

#include <cmath>

namespace iplug { namespace igraphics {

inline const char* FontRegular()  { return orm::UILang() == orm::kLangZH ? "Mixed" : "Outfit"; }
inline const char* FontSemiBold() { return orm::UILang() == orm::kLangZH ? "Mixed-SemiBold" : "Outfit-SemiBold"; }
inline const char* FontBold()     { return orm::UILang() == orm::kLangZH ? "Mixed-Bold" : "Outfit-Bold"; }

// CJK-capable variants, always the synthesized Mixed fonts. Language names are
// conventionally shown in their own script (e.g. the "中文" button stays 中文
// even in the English UI), so those labels must not switch to Outfit.
inline const char* FontCJKRegular()  { return "Mixed"; }
inline const char* FontCJKSemiBold() { return "Mixed-SemiBold"; }
inline const char* FontCJKBold()     { return "Mixed-Bold"; }

// ---- Theme state (live, editable at runtime) ----
// ThemeMode: 0 = light, 1 = dark.
// ThemeHue: 15..360 in steps of 15 (degrees).
// ThemeSatMax: max saturation % used by the ramp formula (0 / 15 / 30 / 50).
inline int& ThemeMode()   { static int mode = 0; return mode; }
inline int& ThemeHue()    { static int hue = 45; return hue; }
inline int& ThemeSatMax() { static int satMax = 15; return satMax; }

// Six brightness stops the palette is built from (B% of HSB).
constexpr int kLightB[5] = { 10, 40, 60, 90, 95 }; // light mode: 900..100
constexpr int kDarkB[5]  = { 90, 60, 40, 10,  5 }; // dark mode: reversed ramp

// Saturation for a given brightness: S = satMax * ((100 - B) / 100)^0.8
inline float SatForB(int B)
{
  return (float) ThemeSatMax() * std::pow((100.f - (float) B) / 100.f, 0.8f);
}

// HSB -> IColor. h in degrees [0,360), s and b in [0,1].
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

// The five ramp tokens, computed live from hue / saturation / theme.
// Light mode walks the last five brightness stops (B10..B95), dark mode
// walks the first five reversed (B90..B5) - same hue family, inverted ramp.
inline IColor COL_900() { const int B = ThemeMode() ? kDarkB[0] : kLightB[0]; return HSBToIColor(ThemeHue(), SatForB(B) / 100.f, B / 100.f); }
inline IColor COL_700() { const int B = ThemeMode() ? kDarkB[1] : kLightB[1]; return HSBToIColor(ThemeHue(), SatForB(B) / 100.f, B / 100.f); }
inline IColor COL_500() { const int B = ThemeMode() ? kDarkB[2] : kLightB[2]; return HSBToIColor(ThemeHue(), SatForB(B) / 100.f, B / 100.f); }
inline IColor COL_300() { const int B = ThemeMode() ? kDarkB[3] : kLightB[3]; return HSBToIColor(ThemeHue(), SatForB(B) / 100.f, B / 100.f); }
inline IColor COL_100() { const int B = ThemeMode() ? kDarkB[4] : kLightB[4]; return HSBToIColor(ThemeHue(), SatForB(B) / 100.f, B / 100.f); }

// Map a 0..255 gray step onto the live warm hue: same saturation formula as
// the ramp tokens, so the band background shares hue and saturation. In dark
// mode the input step is mirrored (255 - v) to walk the dark end.
inline IColor WarmGray(int v)
{
  const int vv = ThemeMode() ? 255 - v : v;
  const float b = vv / 255.f;                       // brightness 0..1
  const int B = (int) std::lround(b * 100.f);       // brightness 0..100 for the formula
  const float s = SatForB(B) / 100.f;               // saturation 0..1
  return HSBToIColor(ThemeHue(), s, b);
}

static constexpr float BLOCK_GAP = 1.5f;
static constexpr float HANDLE_R = 7.f;
static constexpr float HANDLE_RING = 1.5f;
static constexpr float LABEL_VALUE_GAP = 12.f;

// Corner identifiers shared by the XY pad (CENTER / BANDWIDTH) and the band
// range slider (LOWCUT / HIGHCUT) text entries.
enum EPadCorner : int
{
  kCornerCenter = 4,
  kCornerBw     = 5,
  kCornerLow    = 6,
  kCornerHigh   = 7
};

// Selectable band-pass rolloff (slope) presets in dB/oct. Each option equals
// 12 dB/oct per cascaded 2nd-order SVF stage. Default is 96 dB/oct.
constexpr int kSlopeDb[4] = { 12, 24, 48, 96 };
constexpr int kSlopeDefaultIdx = 3; // 96 dB/oct

} }
