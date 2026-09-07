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

  // 无参数版可指定方向（示波器内嵌滑块等纯 UI 场景）
  ORMSlider(const IRECT &bounds, IActionFunction aF, const char *label, const IVStyle &style, EDirection dir)
      : IVSliderControl(bounds, aF, label, style, false, dir), mHeaderLabel(label ? label : "") {
    mStyle.showLabel = false;
    mStyle.showValue = false;
  }

  // 隐藏表头（label/数值），滑块完整占用控件矩形；内嵌小滑块用
  void SetHeaderVisible(bool v) { mHeaderVisible = v; SetDirty(false); }

  // 轨道贴控件远端边 (垂直贴右缘 / 水平贴顶缘): 示波器内嵌滑块用, 轨道与画区边缘齐平, 圆形把手允许压到画区上
  void SetTrackFar(bool v) { mTrackFar = v; SetDirty(false); }

  // 把手悬出扩边: iPlug2 区域渲染把每个控件裁剪到自身矩形, 悬出到画区上的把手部分若不在
  // 控件矩形内就永远画不出来 (视觉上像被背景盖住)。调用后控件矩形向悬出侧扩边 knobR,
  // mOverhang/mStrip 记录扩边量与滑条本体矩形, 供 OnResize/绘制/命中判定使用; 配合 SetTrackFar 使用
  void EnableOverhang(float knobR) {
    mOverhang = knobR;
    if (mDirection == EDirection::Horizontal)
      mRECT.T -= knobR; // 水平轨道贴顶缘: 把手向上悬出画区
    else
      mRECT.R += knobR; // 垂直轨道贴右缘: 把手向右悬出画区
    OnResize();
  }

  void SetValueFormatter(std::function<void(WDL_String &)> f) { mValueFormatter = std::move(f); }
  void SetHeaderLabel(const char *s) {
    mHeaderLabel.Set(s);
    SetDirty(false);
  }

  void OnResize() override {
    if (!mHeaderVisible) {
      // 无表头: 行程/轨道基于滑条本体矩形; 控件矩形可能已向把手悬出侧扩边 (mOverhang)
      if (mOverhang > 0.f) {
        mStrip = (mDirection == EDirection::Horizontal)
                     ? IRECT(mRECT.L, mRECT.T + mOverhang, mRECT.R, mRECT.B)
                     : IRECT(mRECT.L, mRECT.T, mRECT.R - mOverhang, mRECT.B);
      }
      const IRECT &strip = (mStrip.W() > 0.f) ? mStrip : mRECT;
      mWidgetBounds = strip;
      if (mDirection == EDirection::Horizontal) {
        const IRECT travel = strip.GetPadded(-mHandleSize); // 把手行程: 左右各留把手半径
        mTrackBounds = mTrackFar ? IRECT(travel.L, strip.T, travel.R, strip.T + mTrackSize)
                                 : travel.GetMidVPadded(mTrackSize);
      } else {
        const IRECT travel = strip.GetPadded(-mHandleSize); // 把手行程: 上下各留把手半径
        mTrackBounds = mTrackFar ? IRECT(strip.R - mTrackSize, travel.T, strip.R, travel.B)
                                 : travel.GetMidHPadded(mTrackSize);
      }
      SetTargetRECT(mRECT);
      mValueBounds = IRECT();
      SetDirty(false);
      return;
    }
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
    // 背景只填滑条本体: 悬出扩边区覆盖在画区上, 不能抹掉画区内容
    g.FillRect(COL_100(), (mStrip.W() > 0.f) ? mStrip : mRECT);
    DrawWidget(g);
    if (mHeaderVisible)
      DrawHeader(g, mDirection == EDirection::Horizontal ? 0.f : -90.f);
  }

  void OnMouseDown(float x, float y, const IMouseMod &mod) override {
    if (mod.R)
      return;
    if (mHeaderVisible && mod.L && !mod.A && ValueRect().Contains(x, y)) {
      if (GetParam())
        PromptUserInput(ValueRect());
      return;
    }
    // 悬出扩边区(画区上): 仅把手附近响应, 避免贴着画区边缘点击时误改滑块值
    if (mStrip.W() > 0.f && !mStrip.Contains(x, y)) {
      const IRECT filledTrack = mTrackBounds.FracRect(mDirection, (float)GetValue());
      const float kcx = (mDirection == EDirection::Vertical) ? filledTrack.MW() : filledTrack.R;
      const float kcy = (mDirection == EDirection::Vertical) ? filledTrack.T : filledTrack.MH();
      const float dx = x - kcx, dy = y - kcy;
      if (dx * dx + dy * dy > 10.5f * 10.5f) // 把手外缘半径 8.5 + 2 余量
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
    case kLevelHold:
      std::snprintf(buf, sizeof(buf), "%.1fs", p->Value());
      ds.Set(buf);
      break;
    case kRes: {
      const int idx = (int)std::clamp(p->Value(), 0.0, (double)kNumResOptions - 1);
      std::snprintf(buf, sizeof(buf), "%d", kResOptions[idx]);
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
  bool mHeaderVisible = true;
  bool mTrackFar = false;
  float mOverhang = 0.f; // 把手悬出扩边量, 0 = 未扩边
  IRECT mStrip;          // 滑条本体矩形 (扩边后排除悬出区)
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
