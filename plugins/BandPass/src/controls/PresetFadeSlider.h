#pragma once

#include "IControls.h"
#include "../Theme.h"
#include "../Params.h"
#include "UiUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <utility>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class PresetFadeSlider : public IVSliderControl {
public:
  PresetFadeSlider(const IRECT &bounds, IActionFunction aF, const IVStyle &style)
      : IVSliderControl(bounds, aF, "", style, false, EDirection::Horizontal) {}

  void Draw(IGraphics &g) override {
    g.FillRect(COL_100(), mRECT);
    DrawWidget(g);
  }

  void DrawTrack(IGraphics &g, const IRECT &filledArea) override {
    const float cr = GetRoundedCornerRadius(mTrackBounds);
    const IRECT tb = mTrackBounds.GetHPadded(mHandleSize);
    g.FillRoundRect(COL_300(), tb, cr, &mBlend);
    const IRECT fill(tb.L, filledArea.T, std::max(filledArea.R, mTrackBounds.L), filledArea.B);
    g.FillRoundRect(COL_500(), fill, cr, &mBlend);

    const float x0 = mTrackBounds.L, w = mTrackBounds.W();
    for (int i = 0; i < kNumQuick; ++i) {
      const float x = x0 + w * i / (kNumQuick - 1.f);
      g.FillRect(COL_500(), IRECT(x - 1.f, mTrackBounds.T - 3.f, x + 1.f, mTrackBounds.T));
      g.FillRect(COL_500(), IRECT(x - 1.f, mTrackBounds.B, x + 1.f, mTrackBounds.B + 3.f));
    }
  }
  void DrawHandle(IGraphics &g, const IRECT &bounds) override {
    const float cx = bounds.MW(), cy = bounds.MH();
    DrawKnob(g, cx, cy);
  }
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
