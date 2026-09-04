#pragma once

// 扁平按钮族: 瞬时按钮 / 反色开关 / 普通开关

#include "IControls.h"
#include "../Theme.h"

#include <cstdint>
#include <functional>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class FlatActionButton : public IVButtonControl {
public:
  FlatActionButton(const IRECT &bounds, IActionFunction aF, const char *label, const IVStyle &style)
      : IVButtonControl(bounds, aF, label, style) {}

  void Draw(IGraphics &g) override {
    // 用 mRECT 全幅绘制，避免 GetWidgetBounds 因 frame 内缩导致尺寸不一致
    const IRECT b = mRECT;
    const bool pressed = GetValue() > 0.5;
    g.FillRect(pressed ? COL_900() : COL_300(), b);
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

// 小图标按钮 (撤销/重做/保存/读取等窄按钮用图标代替文字)
enum IconAction {
  kIconUndo, // 撤销
  kIconRedo, // 重做
  kIconSave, // 保存 (导出到磁盘)
  kIconLoad, // 读取 (从磁盘导入)
};

// SVG 路径命令表 (源: assets/icons/*.svg, 1024×1024 网格)。
// 命令: 0=Move, 1=Line, 2=贝塞尔(c1x,c1y,c2x,c2y,x,y), 3=Close。圆弧已转成两条三次贝塞尔。
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
static constexpr SvgPathCmd kImportIcon[] = {
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
static constexpr SvgPathCmd kExportIcon[] = {
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

  void OnMouseDown(float x, float y, const IMouseMod &mod) override {
    if (mod.L && !mod.A)
      mFn(this);
  }

  void Draw(IGraphics &g) override {
    const IRECT b = mRECT;
    g.FillRect(COL_300(), b);
    if (GetMouseIsOver())
      g.FillRect(HoverOverlay(), b);

    // 图标从 1024×1024 SVG 网格缩放到 16px，以按钮中心对齐
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
      case kIconSave: path = kExportIcon; nCmds = (int)(sizeof(kExportIcon) / sizeof(kExportIcon[0])); break;
      case kIconLoad: path = kImportIcon; nCmds = (int)(sizeof(kImportIcon) / sizeof(kImportIcon[0])); break;
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

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
