#pragma once

#include "IGraphics.h"

namespace iplug { namespace igraphics {

static const IColor COL_BG    (255, 255, 255, 255);
static const IColor COL_BLACK (255,   0,   0,   0);
static const IColor COL_DIM   (255, 102, 102, 102);
static const IColor COL_FAINT (255, 153, 153, 153);
static const IColor COL_TRACK (255, 243, 243, 243);
static const IColor COL_BLOCK (255, 232, 232, 232);
static const IColor COL_HOVER (255, 166, 166, 166);
static const IColor COL_ACCENT(255,  56,  56,  56);

static constexpr float BLOCK_GAP = 1.5f;
static constexpr float HANDLE_R = 7.f;
static constexpr float HANDLE_RING = 1.5f;

// Corner identifiers shared by the XY pad (CENTER / BANDWIDTH) and the band
// range slider (LOWCUT / HIGHCUT) text entries.
enum EPadCorner : int
{
  kCornerCenter = 4,
  kCornerBw     = 5,
  kCornerLow    = 6,
  kCornerHigh   = 7
};

} }
