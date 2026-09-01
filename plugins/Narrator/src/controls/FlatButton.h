#pragma once

// 扁平按钮族: FlatActionButton (瞬时按钮) / InvertToggleControl (反色开关) / FlatToggleControl。
// MakeMomentary() 为瞬时按钮封装: 点击回调后立即复位值。

#include "IControls.h"
#include "../Theme.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

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

// 扁平分段选择器 (N 选一): TEXT/PHONEMES 等互斥模式。
// active 索引由外部持有 (回调写回), 控件不绑参数。
class FlatSegmentControl : public IControl {
public:
  FlatSegmentControl(const IRECT &bounds, std::vector<std::string> labels,
                     std::function<void(int)> onPick, int activeIdx, float fontSize = 20.f)
      : IControl(bounds), mLabels(std::move(labels)), mOnPick(std::move(onPick)), mActive(activeIdx),
        mFontSize(fontSize) {}

  void SetActive(int idx) {
    if (mActive == idx)
      return;
    mActive = idx;
    SetDirty(false);
  }

  void SetLabels(const std::vector<std::string> &labels) {
    mLabels = labels;
    SetDirty(false);
  }

  void OnMouseDown(float x, float y, const IMouseMod &mod) override {
    if (mod.R)
      return;
    const int n = (int)mLabels.size();
    if (n == 0)
      return;
    const float w = mRECT.W() / n;
    const int idx = std::clamp((int)((x - mRECT.L) / w), 0, n - 1);
    if (idx != mActive) {
      mActive = idx;
      SetDirty(false);
      if (mOnPick)
        mOnPick(idx);
    }
  }

  void Draw(IGraphics &g) override {
    const IRECT b = mRECT;
    const int n = (int)mLabels.size();
    if (n == 0)
      return;
    const float w = b.W() / n;
    for (int i = 0; i < n; ++i) {
      const IRECT raw(b.L + i * w, b.T, b.L + (i + 1) * w, b.B);
      const IRECT seg = raw.GetPadded(-BLOCK_GAP);
      const bool active = i == mActive;
      const bool hover = GetMouseIsOver() && SegmentHit(i, b, w);
      const IColor fill = active ? COL_900() : hover ? COL_500() : COL_300();
      g.FillRect(fill, seg);
      IText t(mFontSize, active ? COL_100() : COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Middle);
      g.DrawText(t, mLabels[i].c_str(), seg);
    }
  }

private:
  bool SegmentHit(int idx, const IRECT &b, float w) const {
    return mMouseX >= b.L + idx * w && mMouseX < b.L + (idx + 1) * w;
  }

  std::vector<std::string> mLabels;
  std::function<void(int)> mOnPick;
  int mActive = 0;
  float mFontSize = 20.f;
  float mMouseX = -1.f;

public:
  void OnMouseOver(float x, float y, const IMouseMod &mod) override {
    mMouseX = x;
    SetDirty(false);
  }
  void OnMouseOut() override {
    mMouseX = -1.f;
    SetDirty(false);
  }
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
