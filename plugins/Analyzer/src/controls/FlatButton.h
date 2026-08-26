#pragma once

// 扁平按钮族: FlatActionButton (瞬时按钮) / InvertToggleControl (反色开关) / FlatToggleControl。
// MakeMomentary() 为瞬时按钮封装: 点击回调后立即复位值。

#include "IControls.h"
#include "../Theme.h"

#include <algorithm>
#include <functional>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class FlatActionButton : public IVButtonControl {
public:
  FlatActionButton(const IRECT &bounds, IActionFunction aF, const char *label, const IVStyle &style)
      : IVButtonControl(bounds, aF, label, style) {}

  void Draw(IGraphics &g) override {
    const IRECT b = GetWidgetBounds();
    const bool pressed = GetValue() > 0.5;
    const IColor fill = pressed ? COL_900() : GetMouseIsOver() ? COL_500() : COL_300();
    g.FillRect(fill, b);
    IText t = mStyle.valueText;
    t.mFGColor = pressed ? COL_100() : COL_900();
    strcpy(t.mFont, kFontSemiBold);
    g.DrawText(t, mLabelStr.Get(), b);
  }
};

inline IVButtonControl *MakeMomentary(const IRECT &r, std::function<void(IControl *)> fn, const char *label,
                                      const IVStyle &st) {
  return new FlatActionButton(
      r,
      [fn](IControl *p) {
        fn(p);
        p->SetValue(0.0);
        p->SetDirty(false);
      },
      label, st);
}

class InvertToggleControl : public IVToggleControl {
public:
  InvertToggleControl(const IRECT &bounds, int paramIdx, const char *label, const IVStyle &style, const char *offText,
                      const char *onText)
      : IVToggleControl(bounds, paramIdx, label, style, offText, onText) {
    SetActionFunction(EmptyClickActionFunc);
  }

  void SetOnText(const char *s) {
    mOnText.Set(s);
    SetDirty(false);
  }
  void SetOffText(const char *s) {
    mOffText.Set(s);
    SetDirty(false);
  }

  void DrawValue(IGraphics &g, bool) override {
    const bool on = GetValue() > 0.5;
    IText t = mStyle.valueText;
    t.mFGColor = on ? COL_100() : COL_900();
    strcpy(t.mFont, kFontSemiBold);
    g.DrawText(t, on ? mOnText.Get() : mOffText.Get(), mWidgetBounds, &mBlend);
  }
};

class FlatToggleControl : public InvertToggleControl {
public:
  using InvertToggleControl::InvertToggleControl;

  void Draw(IGraphics &g) override {
    const IRECT b = GetWidgetBounds();
    const bool on = GetValue() > 0.5;
    const IColor fill = on ? COL_900() : GetMouseIsOver() ? COL_500() : COL_300();
    g.FillRect(fill, b);
    DrawValue(g, false);
  }
};

class FlatCycleButton : public IControl {
public:
  FlatCycleButton(const IRECT &bounds, int paramIdx, const std::vector<const char *> &labels, const IVStyle &style,
                  bool splitChannels = false)
      : IControl(bounds, paramIdx), mLabels(labels), mStyle(style), mSplitChannels(splitChannels) {
    SetActionFunction(EmptyClickActionFunc);
  }

  void OnMouseDown(float x, float y, const IMouseMod &mod) override {
    if (mod.L && !mod.A && GetParam() && !mLabels.empty()) {
      const int num = (int)mLabels.size();
      const int cur = (int)std::clamp(std::lround(GetParam()->Value()), 0L, (long)num - 1);
      const int next = (cur + 1) % num;
      const double norm = (num > 1) ? (double)next / (double)(num - 1) : 0.0;
      SetValueFromUserInput(norm);
    }
  }

  // 与界面背景色同灰度的按钮文字色 (浅色主题接近白但非纯白, 深色主题对应变深)
  static IColor SplitBtnTextColor() {
    const IColor bg = COL_100();
    const int g = std::max({bg.R, bg.G, bg.B});
    return IColor(255, g, g, g);
  }

  void Draw(IGraphics &g) override {
    const IRECT b = mRECT;
    const int num = (int)mLabels.size();
    const int idx = GetParam() ? (int)std::clamp(std::lround(GetParam()->Value()), 0L, (long)num - 1) : 0;

    if (mSplitChannels) {
      // 通道分半样式: 索引 0 = L/R 左半 L 色右半 R 色 (无斜杠, L/R 各半居中);
      // 其余索引 = 整块 M 色 + 居中标签。文字用背景同灰度色。
      IColor cL, cR, cM;
      GetChannelColors(cL, cR, cM);
      const IText wt(20, SplitBtnTextColor(), kFontSemiBold, EAlign::Center, EVAlign::Middle);
      const IRECT pb = b;
      if (idx == 0) {
        const IRECT hl(pb.L, pb.T, pb.MW(), pb.B);
        const IRECT hr(pb.MW(), pb.T, pb.R, pb.B);
        g.FillRect(cL, hl);
        g.FillRect(cR, hr);
        g.DrawText(wt, "L", hl);
        g.DrawText(wt, "R", hr);
      } else {
        g.FillRect(cM, pb);
        if (idx < num)
          g.DrawText(wt, mLabels[idx], pb);
      }
      return;
    }

    const IColor fill = GetMouseIsOver() ? COL_500() : COL_300();
    g.FillRect(fill, b);
    IText t = mStyle.valueText;
    t.mFGColor = COL_900();
    strcpy(t.mFont, kFontSemiBold);
    if (idx >= 0 && idx < num)
      g.DrawText(t, mLabels[idx], b);
  }

private:
  std::vector<const char *> mLabels;
  IVStyle mStyle;
  bool mSplitChannels = false;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
