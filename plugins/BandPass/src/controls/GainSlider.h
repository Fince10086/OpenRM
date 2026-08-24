#pragma once

#include "ORMSlider.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <utility>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class GainSlider : public ORMSlider {
public:
  GainSlider(const IRECT &bounds, int paramIdx, const char *label, const IVStyle &style)
      : ORMSlider(bounds, paramIdx, label, style, EDirection::Vertical) {}

  void SetRandomDeltaDb(float db) {
    if (std::fabs(db - mRandomGainDb) < 1e-3f)
      return;
    mRandomGainDb = db;
    UpdateRandomGhost();
    SetDirty(false);
  }

protected:
  static constexpr float kTextW = 16.f;
  static constexpr float kTextGap = 6.f;
  static constexpr float kPlotTopInset = 30.f;

  double GhostDelta() const override { return (double)mRandomGainDb; }

  void FormatValue(WDL_String &ds) const override {
    const IParam *p = GetParam();
    if (!p) {
      ds.Set("");
      return;
    }
    double v = p->Value();
    if (mRandomMapOn)
      v = std::clamp(v + (double)mRandomGainDb, p->GetMin(), p->GetMax());
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fdB", v);
    ds.Set(buf);
  }

  IRECT TextRect() const { return IRECT(mRECT.R - kTextW - kTextGap, mRECT.T, mRECT.R - kTextGap, mRECT.B); }

  float TrackVisTop() const { return mRECT.T + kPlotTopInset; }

  void OnResize() override {
    mWidgetBounds = mRECT.GetReducedFromRight(kTextW + kTextGap);
    mTrackBounds = mWidgetBounds.GetReducedFromTop(kPlotTopInset + mHandleSize)
                       .GetReducedFromBottom(mHandleSize)
                       .GetMidHPadded(mTrackSize);
    SetTargetRECT(mRECT);
    mValueBounds = IRECT();
    SetDirty(false);
  }

  IRECT ValueRect() const override {
    const IRECT hdr = TextRect();
    return IRECT(hdr.L - 4.f, TrackVisTop(), hdr.R, hdr.MH());
  }

  void DrawHeader(IGraphics &g, float) override {
    WDL_String ds;
    FormatValue(ds);

    const IRECT hdr = TextRect();

    IRECT m;
    g.MeasureText(IText(20, COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Middle), mHeaderLabel.Get(), m);
    const float titleH = m.W();
    const float swatchCY = hdr.B - titleH - AG_SWATCH_GAP - AG_SWATCH * 0.5f;
    mRandomSwatchRect = IRECT(hdr.MW() - AG_SWATCH * 0.5f, swatchCY - AG_SWATCH * 0.5f, hdr.MW() + AG_SWATCH * 0.5f,
                              swatchCY + AG_SWATCH * 0.5f);
    g.FillRect(mRandomMapOn ? RandomColor(mRandomMapColor) : RandomColorDim(mRandomMapColor),
               mRandomSwatchRect);
    g.DrawText(IText(20, COL_700(), kFontRegular, EAlign::Center, EVAlign::Top, 90.f), ds.Get(),
               IRECT(hdr.L, TrackVisTop(), hdr.R, hdr.B));
    g.DrawText(IText(20, COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Bottom, 90.f), mHeaderLabel.Get(), hdr);
  }

  float mRandomGainDb = 0.f;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
