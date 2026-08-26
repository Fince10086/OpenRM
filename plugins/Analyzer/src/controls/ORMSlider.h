#pragma once

// ORMSlider — 自定义主题滑块控件，集成参数名称与数值格式化显示

#include "IControls.h"
#include "../Theme.h"
#include "../Params.h"
#include "../Strings.h"
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
    DrawHeader(g, mDirection == EDirection::Horizontal ? 0.f : -90.f);
  }

  void OnMouseDown(float x, float y, const IMouseMod &mod) override {
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
    const float cx = bounds.MW(), cy = bounds.MH();
    DrawKnob(g, cx, cy);
  }

protected:
  static constexpr float kHeaderH = 26.f;
  static constexpr float kHeaderW = 26.f;

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
    case kRelease:
      std::snprintf(buf, sizeof(buf), "%.2fs", p->Value());
      ds.Set(buf);
      break;
    case kLevelHold:
      std::snprintf(buf, sizeof(buf), "%.1fs", p->Value());
      ds.Set(buf);
      break;
    case kAttack:
      std::snprintf(buf, sizeof(buf), "%.3fs", p->Value());
      ds.Set(buf);
      break;
    case kRes: {
      const int idx = (int)std::clamp(p->Value(), 0.0, (double)kNumResOptions - 1);
      std::snprintf(buf, sizeof(buf), "%d", kResOptions[idx]);
      ds.Set(buf);
      break;
    }
    case kLfRes: {
      const int idx = (int)std::clamp(p->Value(), 0.0, (double)kNumLfResOptions - 1);
      ds.Set(orm::Tr(orm::kTxtLfLow + idx, orm::UILang()));
      break;
    }
    case kBpo: {
      const int idx = (int)std::clamp(p->Value(), 0.0, (double)kNumBpoOptions - 1);
      std::snprintf(buf, sizeof(buf), "%d", kBpoOptions[idx]);
      ds.Set(buf);
      break;
    }
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
      g.DrawText(IText(20, COL_900(), mHeaderFont, EAlign::Near, EVAlign::Middle), mHeaderLabel.Get(),
                 IRECT(hdr.L, hdr.T, hdr.MW(), hdr.B));
      g.DrawText(IText(20, COL_700(), kFontRegular, EAlign::Far, EVAlign::Middle), ds.Get(),
                 IRECT(hdr.MW(), hdr.T, hdr.R, hdr.B));
    } else {
      const IRECT hdr(mRECT.L, mRECT.T, mRECT.L + kHeaderW, mRECT.B);
      g.DrawText(IText(20, COL_900(), kFontSemiBold, EAlign::Near, EVAlign::Bottom, rot), mHeaderLabel.Get(), hdr);
      g.DrawText(IText(20, COL_700(), kFontRegular, EAlign::Far, EVAlign::Top, rot), ds.Get(), hdr);
    }
  }

  WDL_String mHeaderLabel;
  const char *mHeaderFont = kFontSemiBold;
  std::function<void(WDL_String &)> mValueFormatter;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
