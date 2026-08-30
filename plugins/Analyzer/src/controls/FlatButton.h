#pragma once

// 扁平按钮族: FlatActionButton (瞬时按钮) / InvertToggleControl (反色开关) / FlatToggleControl。
// MakeMomentary() 为瞬时按钮封装: 点击回调后立即复位值。

#include "IControls.h"
#include "../Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
    // 全幅绘制, 与 FlatToggleControl/FlatCycleButton (mRECT) 保持一致统一尺寸;
    // GetWidgetBounds 会因 btnStyle.drawFrame 在 MakeRects 里被 GetAdjustedHandleBounds
    // 内缩 0.5*frameThickness (每边 1px), 使动作按钮比开关/循环按钮小一圈。
    const IRECT b = mRECT;
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

// ── 小图标按钮 (文字放不下的窄按钮) ────────────────────────────────────────
// 撤销/重做/保存/读取四个动作改用图标表达, 图标全部由主题色矩形与正圆圆弧
// (多段线逼近) 拼成, 延续界面的矩形+正圆极简语言; 颜色规则与 FlatActionButton
// 一致 (COL_300 底 / COL_900 图标 / hover 叠层, 主题自适应)。

enum IconAction {
  kIconUndo, // 撤销: assets/icons/undo.svg (环形箭头, 实体填充)
  kIconRedo, // 重做: assets/icons/redo.svg (水平镜像)
  kIconSave, // 保存: assets/icons/import.svg (存入容器)
  kIconLoad, // 读取: assets/icons/export.svg (从容器取出)
};

// SVG 路径命令表 (源: assets/icons/*.svg, 1024×1024 网格, 实心填充)。
// 命令: 0=Move, 1=Line, 2=贝塞尔(c1x,c1y,c2x,c2y,x,y), 3=Close。圆弧已按 SVG
// 端点参数化转成两条三次贝塞尔 (180° 半圆, k=0.5523r)。绘制时以主题色填充,
// 保持深/浅主题与色相自适应 (与 FlatActionButton 的 hover/pressed 规则一致)。
struct SvgPathCmd {
  uint8_t cmd;
  float v[6];
};
static constexpr SvgPathCmd kUndoIcon[] = {
    {0, {245.863f, 54.752f}},        {1, {0.f, 300.615f}},
    {1, {245.863f, 546.504f}},       {1, {321.531f, 470.836f}},
    {1, {204.725f, 354.056f}},       {1, {662.909f, 354.056f}},
    {2, {803.245f, 354.056f, 917.010f, 467.821f, 917.010f, 608.157f}},
    {2, {917.010f, 748.493f, 803.245f, 862.258f, 662.909f, 862.258f}},
    {1, {328.566f, 862.258f}},       {1, {328.566f, 969.248f}},
    {1, {662.909f, 969.248f}},       {2, {862.338f, 969.248f, 1024.f, 807.586f, 1024.f, 608.157f}},
    {2, {1024.f, 408.728f, 862.338f, 247.066f, 662.909f, 247.066f}},
    {1, {204.859f, 247.066f}},       {1, {321.505f, 130.394f}},
    {1, {245.863f, 54.752f}},        {3, {}},
};
static constexpr SvgPathCmd kRedoIcon[] = {
    {0, {778.137f, 54.752f}},        {1, {1024.f, 300.615f}},
    {1, {778.137f, 546.504f}},       {1, {702.469f, 470.836f}},
    {1, {819.275f, 354.056f}},       {1, {361.091f, 354.056f}},
    {2, {220.755f, 354.056f, 106.990f, 467.821f, 106.990f, 608.157f}},
    {2, {106.990f, 748.493f, 220.755f, 862.258f, 361.091f, 862.258f}},
    {1, {695.434f, 862.258f}},       {1, {695.434f, 969.248f}},
    {1, {361.091f, 969.248f}},       {2, {161.662f, 969.248f, 0.f, 807.586f, 0.f, 608.157f}},
    {2, {0.f, 408.728f, 161.662f, 247.066f, 361.091f, 247.066f}},
    {1, {819.141f, 247.066f}},       {1, {702.495f, 130.394f}},
    {1, {778.137f, 54.752f}},        {3, {}},
};
static constexpr SvgPathCmd kImportIcon[] = { // 保存: 文件夹 + 下箭头引入托盘
    {0, {849.750f, 419.711f}},       {1, {588.934f, 419.711f}},
    {1, {549.228f, 419.711f}},       {1, {549.228f, 96.167f}},
    {1, {174.250f, 96.167f}},        {1, {174.250f, 907.904f}},
    {1, {463.552f, 907.904f}},       {1, {473.533f, 907.904f}},
    {1, {473.533f, 994.527f}},       {1, {473.533f, 1004.144f}},
    {1, {114.583f, 1004.144f}},      {1, {74.877f, 1004.144f}},
    {1, {74.877f, 38.467f}},         {1, {74.877f, 0.f}},
    {1, {629.805f, 0.f}},            {1, {638.687f, 0.f}},
    {1, {944.023f, 327.623f}},       {1, {949.123f, 333.095f}},
    {1, {949.123f, 567.604f}},       {1, {949.123f, 577.293f}},
    {1, {859.731f, 577.293f}},       {1, {849.750f, 577.293f}},
    {1, {849.750f, 419.711f}},       {3, {}},
    {0, {820.463f, 333.160f}},       {1, {638.693f, 138.204f}},
    {1, {638.693f, 333.160f}},       {1, {820.463f, 333.160f}},
    {3, {}},
    {0, {675.921f, 856.250f}},       {1, {939.215f, 856.250f}},
    {1, {949.123f, 856.250f}},       {1, {949.123f, 769.700f}},
    {1, {949.123f, 760.083f}},       {1, {673.735f, 760.083f}},
    {1, {771.797f, 665.082f}},       {1, {778.828f, 658.271f}},
    {1, {715.626f, 597.037f}},       {1, {708.596f, 590.226f}},
    {1, {512.801f, 779.900f}},       {1, {484.712f, 807.111f}},
    {1, {701.566f, 1017.184f}},      {1, {708.601f, 1024.f}},
    {1, {771.797f, 962.690f}},       {1, {778.785f, 955.910f}},
    {1, {675.921f, 856.250f}},       {3, {}},
};
static constexpr SvgPathCmd kExportIcon[] = { // 读取: 托盘 + 上箭头取出
    {0, {111.884f, 623.884f}},       {1, {111.884f, 890.580f}},
    {1, {111.884f, 917.293f}},       {1, {885.403f, 917.293f}},
    {1, {912.043f, 917.293f}},       {1, {912.043f, 623.884f}},
    {1, {805.408f, 623.884f}},       {1, {778.695f, 623.884f}},
    {1, {778.695f, 543.890f}},       {1, {778.695f, 517.250f}},
    {1, {992.110f, 517.250f}},       {1, {1018.750f, 517.250f}},
    {1, {1018.750f, 997.287f}},      {1, {1018.750f, 1024.f}},
    {1, {31.890f, 1024.f}},          {1, {5.250f, 1024.f}},
    {1, {5.250f, 543.890f}},         {1, {5.250f, 517.250f}},
    {1, {218.664f, 517.250f}},       {1, {245.305f, 517.250f}},
    {1, {245.305f, 597.244f}},       {1, {245.305f, 623.884f}},
    {1, {111.884f, 623.884f}},       {3, {}},
    {0, {565.281f, 202.703f}},       {1, {565.281f, 656.172f}},
    {1, {565.281f, 682.812f}},       {1, {485.359f, 682.812f}},
    {1, {458.646f, 682.812f}},       {1, {458.646f, 202.703f}},
    {1, {303.943f, 357.406f}},       {1, {285.320f, 376.029f}},
    {1, {229.306f, 320.124f}},       {1, {210.613f, 301.467f}},
    {1, {493.323f, 18.680f}},        {1, {511.997f, 0.f}},
    {1, {794.694f, 282.697f}},       {1, {813.390f, 301.392f}},
    {1, {757.340f, 357.334f}},       {1, {738.689f, 375.948f}},
    {1, {565.281f, 202.703f}},       {3, {}},
};

class IconActionButton : public IControl {
public:
  IconActionButton(const IRECT &bounds, std::function<void(IControl *)> fn, IconAction icon)
      : IControl(bounds), mFn(std::move(fn)), mIcon(icon) {}

  // 快速连击: 偶数次点击会被平台识别为双击, 若不处理会走 IControl 默认的
  // SetValueToDefault, 导致丢一次响应。双击视为再次按下。
  void OnMouseDblClick(float x, float y, const IMouseMod &mod) override { OnMouseDown(x, y, mod); }

  void OnMouseDown(float x, float y, const IMouseMod &mod) override {
    if (mod.L && !mod.A)
      mFn(this);
  }

  void Draw(IGraphics &g) override {
    const IRECT b = mRECT;
    g.FillRect(COL_300(), b);
    if (GetMouseIsOver())
      g.FillRect(HoverOverlay(), b);

    // 图标几何来自用户提供的 assets/icons/*.svg (1024×1024 网格), 缩放到 18px
    // 以按钮中心对齐。实心填充主题色, 随深浅主题/色相自适应。
    const float cx = b.MW(), cy = b.MH();
    const float s = 16.f / 1024.f;
    auto X = [&](float vx) { return cx + (vx - 512.f) * s; };
    auto Y = [&](float vy) { return cy + (vy - 512.f) * s; };
    const IColor c = COL_900();

    const SvgPathCmd *path = nullptr;
    int nCmds = 0;
    switch (mIcon) {
      case kIconUndo: path = kUndoIcon; nCmds = (int)(sizeof(kUndoIcon) / sizeof(kUndoIcon[0])); break;
      case kIconRedo: path = kRedoIcon; nCmds = (int)(sizeof(kRedoIcon) / sizeof(kRedoIcon[0])); break;
      case kIconSave: path = kImportIcon; nCmds = (int)(sizeof(kImportIcon) / sizeof(kImportIcon[0])); break;
      case kIconLoad: path = kExportIcon; nCmds = (int)(sizeof(kExportIcon) / sizeof(kExportIcon[0])); break;
    }
    if (path) {
      for (int i = 0; i < nCmds; ++i) {
        const SvgPathCmd &e = path[i];
        switch (e.cmd) {
          case 0: g.PathMoveTo(X(e.v[0]), Y(e.v[1])); break;
          case 1: g.PathLineTo(X(e.v[0]), Y(e.v[1])); break;
          case 2:
            g.PathCubicBezierTo(X(e.v[0]), Y(e.v[1]), X(e.v[2]), Y(e.v[3]), X(e.v[4]), Y(e.v[5]));
            break;
          default: g.PathClose(); break;
        }
      }
      g.PathFill(IPattern(c));
    }
  }

private:
  std::function<void(IControl *)> mFn;
  IconAction mIcon;
};

inline IconActionButton *MakeIconMomentary(const IRECT &r, std::function<void(IControl *)> fn, IconAction icon) {
  return new IconActionButton(r, std::move(fn), icon);
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
    // 分半样式锚点记录标签位置而非字符串: 语言切换后标签变中文, strcmp 判断将失效
    // (创建时刻标签恒为英文, 位置语义稳定)
    mSplitIdx = -1;
    for (int i = 0; i < (int)labels.size(); ++i)
      if (std::strcmp(labels[i], "L/R") == 0)
        mSplitIdx = i;
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
      // 通道分半样式: L/R 档左半 L 色右半 R 色; 其余档整块 M 色 + 居中标签
      IColor cL, cR, cM;
      GetChannelColors(cL, cR, cM);
      const IText wt(20, SplitBtnTextColor(), kFontSemiBold, EAlign::Center, EVAlign::Middle);
      const IRECT pb = b;
      const bool isSplit = (idx >= 0 && idx < num && idx == mSplitIdx);
      if (isSplit) {
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
  int mSplitIdx = -1;   // 分半样式锚点 (标签位置, 见构造注释)
  bool mScaleStyle = false;
  float mTextSize = 0.f; // >0 时覆盖样式字号 (窄按钮场景)
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
