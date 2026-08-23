#pragma once

// 扁平按钮族: FlatActionButton (瞬时按钮) / InvertToggleControl (反色开关) / FlatToggleControl。
// MakeMomentary() 为瞬时按钮封装: 点击回调后立即复位值。

#include "IControls.h"
#include "../Theme.h"

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
    g.FillRect(fill, b.GetPadded(-BLOCK_GAP));
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
    g.FillRect(fill, b.GetPadded(-BLOCK_GAP));
    DrawValue(g, false);
  }
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
