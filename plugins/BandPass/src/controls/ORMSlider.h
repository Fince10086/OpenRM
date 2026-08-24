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

class ORMSlider : public IVSliderControl {
public:
  ORMSlider(const IRECT &bounds, int paramIdx, const char *label, const IVStyle &style, EDirection dir)
      : IVSliderControl(bounds, paramIdx, label, style, false, dir), mHeaderLabel(label ? label : "") {
    mStyle.showLabel = false;
    mStyle.showValue = false;
  }

  ORMSlider(const IRECT &bounds, IActionFunction aF, const char *label, const IVStyle &style)
      : IVSliderControl(bounds, aF, label, style, false, EDirection::Horizontal), mHeaderLabel(label ? label : "") {
    mStyle.showLabel = false;
    mStyle.showValue = false;
  }

  void SetValueFormatter(std::function<void(WDL_String &)> f) { mValueFormatter = std::move(f); }
  void SetHeaderLabel(const char *s) {
    mHeaderLabel.Set(s);
    SetDirty(false);
  }

  void SetGhost(bool ghost) {
    if (mGhost == ghost)
      return;
    mGhost = ghost;
    SetDirty(false);
  }

  struct RandomHooks {
    std::function<void()> randomToggle;
    std::function<void(int colorIdx)> randomSetColor;
  };

  void SetHeaderSwatchColor(int colorIdx) {
    mHeaderSwatchColor = colorIdx;
    SetDirty(false);
  }
  void SetHeaderFont(const char *font) {
    mHeaderFont = font;
    SetDirty(false);
  }
  void SetRandomMapHooks(const RandomHooks &h) { mRandomHooks = h; }
  void SetRandomMapState(bool on, int colorIdx) {
    mRandomMapOn = on;
    mRandomMapColor = colorIdx;
    UpdateRandomGhost();
    SetDirty(false);
  }
  void SetRandomDeltaMix(float d) {
    if (std::fabs(d - mRandomMixDelta) < 1e-4f)
      return;
    mRandomMixDelta = d;
    UpdateRandomGhost();
    SetDirty(false);
  }

  void OnResize() override {

    if (mDirection == EDirection::Horizontal) {
      mWidgetBounds = mRECT.GetReducedFromTop(kHeaderH);
      mTrackBounds = mWidgetBounds.GetPadded(-mHandleSize).GetMidVPadded(mTrackSize);
    } else {
      mWidgetBounds = mRECT.GetReducedFromLeft(kHeaderW);
      mTrackBounds = mWidgetBounds.GetPadded(-mHandleSize).GetMidHPadded(mTrackSize);
    }
    SetTargetRECT(mRECT);
    mValueBounds = IRECT();
    SetDirty(false);
  }

  bool IsHit(float x, float y) const override { return mRECT.Contains(x, y); }

  void Draw(IGraphics &g) override {
    g.FillRect(COL_100(), mRECT);
    DrawWidget(g);
    if (mGhost) {
      DrawGhostOverlay(g, mRECT);
      return;
    }
    DrawHeader(g, mDirection == EDirection::Vertical ? -90.f : 0.f);
  }

  void OnMouseDown(float x, float y, const IMouseMod &mod) override {
    if (mGhost)
      return;
    if (mRandomHooks.randomToggle && mRandomSwatchRect.Contains(x, y)) {
      if (mod.R)
        OpenRandomColorMenu();
      else
        mRandomHooks.randomToggle();
      return;
    }
    if (mod.R)
      return;
    if (mod.L && !mod.A && ValueRect().Contains(x, y)) {
      if (GetParam())
        PromptUserInput(ValueRect());
      return;
    }
    IVSliderControl::OnMouseDown(x, y, mod);
  }

  void OnMouseDblClick(float x, float y, const IMouseMod &mod) override {
    if (mGhost)
      return;
    IVSliderControl::OnMouseDblClick(x, y, mod);
  }

  void DrawTrack(IGraphics &g, const IRECT &filledArea) override {
    const bool horiz = (mDirection == EDirection::Horizontal);
    const float cr = GetRoundedCornerRadius(mTrackBounds);
    const IRECT tb = horiz ? mTrackBounds.GetHPadded(mHandleSize) : mTrackBounds.GetVPadded(mHandleSize);
    g.FillRoundRect(COL_300(), tb, cr, &mBlend);
    const IRECT fill = horiz ? IRECT(tb.L, filledArea.T, std::max(filledArea.R, mTrackBounds.L), filledArea.B)
                             : IRECT(filledArea.L, filledArea.T, filledArea.R, tb.B);
    g.FillRoundRect(COL_500(), fill, cr, &mBlend);
  }

  void DrawHandle(IGraphics &g, const IRECT &bounds) override {
    if (mGhost)
      return;
    if (mRandomMapOn && mRandomGhostNorm >= 0.f) {
      const IRECT tb = mTrackBounds;
      if (mDirection == EDirection::Horizontal) {
        const float x = std::clamp(tb.L + mRandomGhostNorm * tb.W(), tb.L, tb.R);
        g.FillCircle(COL_100(), x, tb.MH(), HANDLE_R + HANDLE_RING);
        g.FillCircle(RandomColorGhost(mRandomMapColor), x, tb.MH(), HANDLE_R);
      } else {
        const float y = std::clamp(tb.B - mRandomGhostNorm * tb.H(), tb.T, tb.B);
        g.FillCircle(COL_100(), tb.MW(), y, HANDLE_R + HANDLE_RING);
        g.FillCircle(RandomColorGhost(mRandomMapColor), tb.MW(), y, HANDLE_R);
      }
    }
    const float cx = bounds.MW(), cy = bounds.MH();
    DrawKnob(g, cx, cy);
  }

protected:
  static constexpr float kHeaderH = 26.f;
  static constexpr float kHeaderW = 26.f;

  void UpdateRandomGhost() {
    const IParam *p = GetParam();
    if (!mRandomMapOn || !p) {
      mRandomGhostNorm = -1.f;
      return;
    }
    const double v = std::clamp(p->Value() + GhostDelta(), p->GetMin(), p->GetMax());
    mRandomGhostNorm = (float)p->ToNormalized(v);
  }
  virtual double GhostDelta() const { return (double)mRandomMixDelta; }

  virtual IRECT ValueRect() const {
    if (mDirection == EDirection::Horizontal)
      return IRECT(mRECT.L, mRECT.T, mRECT.R, mRECT.T + kHeaderH);
    return IRECT(mRECT.L, mRECT.T, mRECT.L + kHeaderW + 4.f, mRECT.T + 44.f);
  }

  virtual void FormatValue(WDL_String &ds) const {
    ds.Set("");
    if (mValueFormatter) {
      mValueFormatter(ds);
      return;
    }
    const IParam *p = GetParam();
    if (!p)
      return;
    char buf[32];
    switch (GetParamIdx()) {
    case kRandomAmountR:
    case kRandomAmountY:
    case kRandomAmountB:
    case kRandomAmountG:
      std::snprintf(buf, sizeof(buf), "%.0f%%", p->Value() * 100.);
      ds.Set(buf);
      break;
    case kMix: {
      double v = p->Value();
      if (mRandomMapOn)
        v = std::clamp(v + (double)mRandomMixDelta, 0., 1.);
      std::snprintf(buf, sizeof(buf), "%.0f%%", v * 100.);
      ds.Set(buf);
      break;
    }
    case kRandomRateR:
    case kRandomRateY:
    case kRandomRateB:
    case kRandomRateG:
      std::snprintf(buf, sizeof(buf), p->Value() < 10. ? "%.2fs" : "%.1fs", p->Value());
      ds.Set(buf);
      break;
    case kGainL:
    case kGainR:
      std::snprintf(buf, sizeof(buf), "%.1fdB", p->Value());
      ds.Set(buf);
      break;
    default:
      p->GetDisplay(ds, false);
      break;
    }
  }

  virtual void DrawHeader(IGraphics &g, float rot) {
    WDL_String ds;
    FormatValue(ds);

    if (rot == 0.f) {
      const IRECT hdr(mRECT.L, mRECT.T, mRECT.R, mRECT.T + kHeaderH);
      float labelL = hdr.L;
      if (mHeaderSwatchColor >= 0) {
        g.FillRect(RandomColor(mHeaderSwatchColor), IRECT(hdr.L + 1.f, hdr.MH() - AG_SWATCH * 0.5f,
                                                          hdr.L + 1.f + AG_SWATCH, hdr.MH() + AG_SWATCH * 0.5f));
        labelL = hdr.L + AG_SWATCH + AG_SWATCH_GAP;
      }
      float valueR = hdr.R;
      if (mRandomHooks.randomToggle) {
        mRandomSwatchRect =
            IRECT(hdr.R - 1.f - AG_SWATCH, hdr.MH() - AG_SWATCH * 0.5f, hdr.R - 1.f, hdr.MH() + AG_SWATCH * 0.5f);
        g.FillRect(mRandomMapOn ? RandomColor(mRandomMapColor) : RandomColorDim(mRandomMapColor),
                   mRandomSwatchRect);
        valueR = mRandomSwatchRect.L - 6.f;
      }
      g.DrawText(IText(20, COL_900(), mHeaderFont, EAlign::Near, EVAlign::Middle), mHeaderLabel.Get(),
                 IRECT(labelL, hdr.T, hdr.MW(), hdr.B));
      g.DrawText(IText(20, COL_700(), kFontRegular, EAlign::Far, EVAlign::Middle), ds.Get(),
                 IRECT(hdr.MW(), hdr.T, valueR, hdr.B));
    } else {
      const IRECT hdr(mRECT.L, mRECT.T, mRECT.L + kHeaderW, mRECT.B);
      g.DrawText(IText(20, COL_900(), kFontSemiBold, EAlign::Near, EVAlign::Bottom, rot), mHeaderLabel.Get(), hdr);
      g.DrawText(IText(20, COL_700(), kFontRegular, EAlign::Far, EVAlign::Top, rot), ds.Get(), hdr);
    }
  }

  void OpenRandomColorMenu() {
    if (!GetUI())
      return;
    OpenColorPopup(*GetUI(), *this, mRandomMenu, mRandomSwatchRect, mRandomMapColor, [this](int idx) {
      if (mRandomHooks.randomSetColor)
        mRandomHooks.randomSetColor(idx);
    });
  }

  WDL_String mHeaderLabel;
  const char *mHeaderFont = kFontSemiBold;
  std::function<void(WDL_String &)> mValueFormatter;
  bool mGhost = false;
  int mHeaderSwatchColor = -1;
  bool mRandomMapOn = false;
  int mRandomMapColor = 0;
  IRECT mRandomSwatchRect;
  IPopupMenu mRandomMenu;
  RandomHooks mRandomHooks;
  float mRandomGhostNorm = -1.f;
  float mRandomMixDelta = 0.f;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
