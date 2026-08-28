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

  // 快速连击: 偶数次点击会被平台识别为双击, 若不处理会走 IControl 默认的
  // SetValueToDefault (重置参数), 导致丢一次响应。双击视为再次按下。
  void OnMouseDblClick(float x, float y, const IMouseMod &mod) override { OnMouseDown(x, y, mod); }

  void Draw(IGraphics &g) override {
    const IRECT b = GetWidgetBounds();
    const bool pressed = GetValue() > 0.5;
    g.FillRect(pressed ? COL_900() : COL_300(), b);
    // hover: 半透明叠层 (替代原换色), pressed 时不叠加
    if (!pressed && GetMouseIsOver())
      g.FillRect(HoverOverlay(), b);
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

  // 快速连击: 偶数次点击走双击, 转发为按下 (切换), 避免重置默认
  void OnMouseDblClick(float x, float y, const IMouseMod &mod) override { OnMouseDown(x, y, mod); }

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
    const IRECT b = mRECT;
    const bool on = GetValue() > 0.5;
    g.FillRect(on ? COL_900() : COL_300(), b);
    // hover: 半透明叠层 (替代原换色), on 时不叠加
    if (!on && GetMouseIsOver())
      g.FillRect(HoverOverlay(), b);
    DrawValue(g, false);
  }

  void DrawValue(IGraphics &g, bool) override {
    const bool on = GetValue() > 0.5;
    IText t = mStyle.valueText;
    t.mFGColor = on ? COL_100() : COL_900();
    strcpy(t.mFont, kFontSemiBold);
    g.DrawText(t, on ? mOnText.Get() : mOffText.Get(), mRECT, &mBlend);
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

  // 快速连击: 偶数次点击走双击, 转发为按下 (循环切换), 避免重置默认
  void OnMouseDblClick(float x, float y, const IMouseMod &mod) override { OnMouseDown(x, y, mod); }

  // 运行时换标签 (模式相关档位按钮: 切引擎时档值含义变化, 由插件在模式切换时调用)
  void SetLabels(const std::vector<const char *> &labels) {
    mLabels = labels;
    SetDirty(false);
  }

  // 刻度样式: 按钮伪装成刻度文字 (如频谱图底部 Range 按钮) —— 背景方块 + 与刻度一致的
  // 14px 文字 (右对齐, 右缘/底缘与刻度文字重合), 只是多出一个背景色块。
  void SetScaleLabelStyle(bool b) { mScaleStyle = b; }

  // 缩小按钮文字: 默认样式 20px 字在受窄的按钮里放不下时使用
  // (如电平条底部的模式覆盖按钮)。<=0 表示沿用样式原字号。
  void SetTextSize(float px) { mTextSize = px; }

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
      // hover: 半透明叠层叠在通道色之上 (通道色也可获得 hover 反馈)
      if (GetMouseIsOver())
        g.FillRect(HoverOverlay(), pb);
      return;
    }

    if (mScaleStyle) {
      // 刻度样式: 加色半透明背景 (EBlend::Add = 线性提亮, 在频谱上形成光晕方块) +
      // 与刻度文字相同的位置/字号/颜色/对齐。文字矩形 = (L+4, T, R-3, B-1):
      // 右缘/底缘与 SpectrumPad DrawDbGrid 的最底部刻度文字完全重合 (kTickRight=3, 贴线留 1px)。
      IBlend add(EBlend::Add, 1.f);
      const IColor fill = GetMouseIsOver() ? IColor(72, 255, 255, 255) : IColor(40, 255, 255, 255);
      g.FillRect(fill, b, &add);
      if (idx >= 0 && idx < num) {
        const IText t(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Bottom);
        g.DrawText(t, mLabels[idx], IRECT(b.L + 4.f, b.T, b.R - 3.f, b.B - 1.f));
      }
      return;
    }

    g.FillRect(COL_300(), b);
    if (GetMouseIsOver())
      g.FillRect(HoverOverlay(), b);
    IText t = mStyle.valueText;
    if (mTextSize > 0.f)
      t.mSize = mTextSize;
    t.mFGColor = COL_900();
    strcpy(t.mFont, kFontSemiBold);
    if (idx >= 0 && idx < num)
      g.DrawText(t, mLabels[idx], b);
  }

private:
  std::vector<const char *> mLabels;
  IVStyle mStyle;
  bool mSplitChannels = false;
  bool mScaleStyle = false;
  float mTextSize = 0.f; // >0 时覆盖样式字号 (窄按钮场景)
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
