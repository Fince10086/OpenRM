#pragma once

#include "IGraphics.h"
#include "Strings.h"

#include <cmath>

namespace iplug { namespace igraphics {

inline const char* FontRegular()  { return orm::UILang() == orm::kLangZH ? "Mixed" : "Outfit"; }
inline const char* FontSemiBold() { return orm::UILang() == orm::kLangZH ? "Mixed-SemiBold" : "Outfit-SemiBold"; }
inline const char* FontBold()     { return orm::UILang() == orm::kLangZH ? "Mixed-Bold" : "Outfit-Bold"; }

// Neutral gray ramp, named by lightness step (900 = darkest, 100 = lightest)
// so the same tokens can be remapped for a future dark mode.
static const IColor COL_900 (255,  26,  25,  22); // #1A1916
static const IColor COL_700 (255, 102,  99,  92); // #66635C
static const IColor COL_500 (255, 153, 150, 142); // #99968E
static const IColor COL_300 (255, 230, 228, 224); // #E6E4E0
static const IColor COL_100 (255, 242, 241, 239); // #F2F1EF

// Map a 0..255 gray step onto the palette's warm hue (H = 45): saturation
// rises as the gray darkens, S = 15 * ((100 - B) / 100)^0.8, with B = v/2.55.
// At v = 26 this reproduces COL_900, so the band background shares the ramp.
inline IColor WarmGray(int v)
{
  const float b = v / 255.f;                        // brightness 0..1
  const float s = 0.15f * std::pow(1.f - b, 0.8f);  // saturation 0..1
  const float c = b * s;                            // chroma
  const float x = c * 0.75f;                        // H=45 -> X = C*(1-|0.75-1|)
  const float m = b - c;
  return IColor(255, (int) std::lround((c + m) * 255.f),
                     (int) std::lround((x + m) * 255.f),
                     (int) std::lround(m * 255.f));
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
