#pragma once

#include "IGraphics.h"

namespace iplug { namespace igraphics {

static const IColor COL_BG    (255, 255, 255, 255);
static const IColor COL_BLACK (255,   0,   0,   0);
static const IColor COL_DIM   (255, 102, 102, 102);
static const IColor COL_FAINT (255, 153, 153, 153);
static const IColor COL_TRACK (255, 236, 236, 236);
static const IColor COL_HOVER (255, 240, 240, 240);
static const IColor COL_GRID  (255, 204, 204, 204);
static const IColor COL_DGRAY (255,  70,  70, 70);

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
