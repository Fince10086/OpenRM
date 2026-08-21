// ============================================================================
// FilterNodePad.h — 黑白极简风格滤波节点可视化控件 (模仿 nono.feizao.org / GRM)
//   白底 + 2px 纯黑边框 + #ccc 细网格 + 实心黑球节点
//   四角可点击编辑的参数: 左上 CENTER / 右上 BANDWIDTH / 左下 LOWCUT / 右下 HIGHCUT
//   网格下方一条双控制点范围滑块 (对应 low/high 截止频率, 两点间涂黑)
//   继承 IVXYPadControl: 内部 valIdx0 = 中心频率 (对数), valIdx1 = 带宽 (oct)
// ============================================================================
#pragma once

#include "IControls.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

// 角标 id 约定 (valIdx 4..7, 0/1 已被 freq/bw 占用)
enum EPadCorner : int
{
  kCornerCenter = 4,
  kCornerBw     = 5,
  kCornerLow    = 6,
  kCornerHigh   = 7
};

// 文件内配色 (黑白极简)
static const IColor FP_BLACK (255,   0,   0,   0);
static const IColor FP_WHITE (255, 255, 255, 255);
static const IColor FP_GRID  (255, 204, 204, 204); // #ccc 细网格
static const IColor FP_TRACK (255, 236, 236, 236); // 滑块底 #ececec
static const IColor FP_HOVER (255, 240, 240, 240); // #f0f0f0 hover

class FilterNodePad : public IVXYPadControl
{
public:
  // 回调: 把用户交互交回插件层换算并写参 (插件持有 param 范围/钳位逻辑)
  struct Hooks
  {
    std::function<void()> gestureBegin;                                  // 滑块手势起点 → MaybePushGestureUndo
    std::function<void(int cornerId, double value)> editCorner;          // 角标文本提交 (value: Hz 或 oct)
    std::function<void(double lowNorm, double highNorm)> editBand;        // 滑块拖点 (归一化 20..20k)
  };

  FilterNodePad(const IRECT& bounds, const std::initializer_list<int>& params,
                const char* label, const IVStyle& style, const Hooks& hooks,
                float handleRadius = 9.f)
  : IVXYPadControl(bounds, params, label, style, handleRadius, true, true)
  , mHooks(hooks)
  {
    SetTextEntryLength(20);
  }

  void Draw(IGraphics& g) override
  {
    IVXYPadControl::Draw(g); // 背景 + 边框 + 标签 + 网格 + 节点
    DrawSlider(g);
    DrawCorner(g, kCornerCenter);
    DrawCorner(g, kCornerBw);
    DrawCorner(g, kCornerLow);
    DrawCorner(g, kCornerHigh);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    mOverCorner = -1;
    // 1) 角标 → 行内文本编辑
    for (int id : { kCornerCenter, kCornerBw, kCornerLow, kCornerHigh })
    {
      if (CornerRect(id).Contains(x, y))
      {
        WDL_String init; GetCornerLabel(id, init);
        EAlign align = (id == kCornerBw || id == kCornerHigh) ? EAlign::Far : EAlign::Near;
        IText t(11, FP_BLACK, "Outfit-SemiBold", align, EVAlign::Middle);
        GetUI()->CreateTextEntry(*this, t, CornerRect(id), init.Get(), id);
        return;
      }
    }
    // 2) 滑块条 → 拖控制点
    if (SliderRect().Contains(x, y))
    {
      const float lx = NormToX(LowNorm()), hx = NormToX(HighNorm());
      const float dL = std::fabs(x - lx), dR = std::fabs(x - hx);
      if (x > lx + 7.f && x < hx - 7.f) { mActiveHandle = 2; mStartX = x; mStartLow = LowNorm(); mStartHigh = HighNorm(); }
      else if (dL <= dR) mActiveHandle = 0;
      else mActiveHandle = 1;
      if (mHooks.gestureBegin) mHooks.gestureBegin();
      OnMouseDrag(x, y, 0.f, 0.f, mod);
      return;
    }
    // 3) 其余 → XY 节点拖拽 (限于绘图区)
    if (PlotRect().Contains(x, y))
    {
      mMouseDown = true;
      NodeDrag(x, y);
    }
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override
  {
    if (mActiveHandle == 0 || mActiveHandle == 1)
    {
      float n = XToNorm(x);
      if (mActiveHandle == 0)
      {
        const float hN = HighNorm();
        n = std::clamp(n, 0.f, hN - kMinGap);
        mHooks.editBand((double) n, (double) hN);
      }
      else
      {
        const float lN = LowNorm();
        n = std::clamp(n, lN + kMinGap, 1.f);
        mHooks.editBand((double) lN, (double) n);
      }
    }
    else if (mActiveHandle == 2)
    {
      const float srx = SliderRect().W();
      const float d = (x - mStartX) / srx;
      float lN = std::clamp(mStartLow + d, 0.f, 1.f - kMinGap);
      float hN = std::clamp(mStartHigh + d, lN + kMinGap, 1.f);
      if (hN - lN < kMinGap) { hN = lN + kMinGap; if (hN > 1.f) { hN = 1.f; lN = 1.f - kMinGap; } }
      mHooks.editBand((double) lN, (double) hN);
    }
    else
    {
      NodeDrag(x, y);
    }
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    mActiveHandle = -1;
    IVXYPadControl::OnMouseUp(x, y, mod);
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override
  {
    int hit = -1;
    for (int id : { kCornerCenter, kCornerBw, kCornerLow, kCornerHigh })
      if (CornerRect(id).Contains(x, y)) { hit = id; break; }
    if (hit != mOverCorner) { mOverCorner = hit; SetDirty(false); }
    IVXYPadControl::OnMouseOver(x, y, mod);
  }

  void OnMouseOut() override
  {
    if (mOverCorner != -1) { mOverCorner = -1; SetDirty(false); }
    IVXYPadControl::OnMouseOut();
  }

  void OnTextEntryCompletion(const char* str, int valIdx) override
  {
    double v;
    if (valIdx == kCornerBw) { char* end = nullptr; v = std::strtod(str, &end); if (end == str) return; }
    else if (!ParseFreq(str, v)) return;
    if (mHooks.editCorner) mHooks.editCorner(valIdx, v);
  }

  // 节点区域限定在绘图区 (基类用 mWidgetBounds 会越过滑块条)
  void DrawWidget(IGraphics& g) override
  {
    DrawTrack(g);
    const IRECT tb = PlotRect();
    const float xpos = (float) GetValue(0) * tb.W();
    const float ypos = (float) GetValue(1) * tb.H();
    const IRECT hb(tb.L + xpos - mHandleRadius, tb.B - ypos - mHandleRadius,
                   tb.L + xpos + mHandleRadius, tb.B - ypos + mHandleRadius);
    DrawHandle(g, tb, hb);
  }

  // 网格限定在绘图区 (滑块/角标之上)
  void DrawTrack(IGraphics& g) override
  {
    const IRECT tb = PlotRect();
    g.FillRect(FP_WHITE, tb);
    for (int i = 1; i < 4; ++i)
    {
      const float y = tb.T + tb.H() * i / 4.f;
      g.DrawLine(FP_GRID, tb.L, y, tb.R, y, nullptr, 1.f);
      const float x = tb.L + tb.W() * i / 4.f;
      g.DrawLine(FP_GRID, x, tb.T, x, tb.B, nullptr, 1.f);
    }
  }

  // 节点限定在绘图区
  void DrawHandle(IGraphics& g, const IRECT& trackBounds, const IRECT& handleBounds) override
  {
    const float cx = handleBounds.MW();
    const float cy = handleBounds.MH();
    const float r  = handleBounds.W() * 0.5f;
    g.DrawLine(FP_BLACK, cx, handleBounds.T, cx, cy - r, nullptr, 1.f);
    g.FillCircle(FP_BLACK, cx, cy, r);
    g.FillCircle(FP_WHITE, cx, cy, r * 0.25f);
  }

private:
  IRECT PlotRect() const
  {
    const IRECT& w = mWidgetBounds;
    const float top = w.T + kTopPad;
    const float srBottom = w.B - kGap - kCutRowH - kGap - kSliderH;
    return IRECT(w.L, top, w.R, srBottom);
  }

  IRECT SliderRect() const
  {
    const IRECT& w = mWidgetBounds;
    const float top = w.B - kGap - kCutRowH - kGap - kSliderH;
    return IRECT(w.L + 2.f, top, w.R - 2.f, top + kSliderH);
  }

  IRECT CutRowRect() const
  {
    const IRECT& w = mWidgetBounds;
    return IRECT(w.L, w.B - kGap - kCutRowH, w.R, w.B - kGap);
  }

  IRECT CornerRect(int id) const
  {
    const IRECT& w = mWidgetBounds;
    const float ty = w.T + 2.f;
    const float by = w.T + 2.f + kCornerH;
    const float cy = w.B - kGap - kCutRowH; // 改用 CutRowRect 同基线
    switch (id)
    {
      case kCornerCenter: return IRECT(w.L + kInsetX, ty, w.L + kInsetX + kCornerW, by);
      case kCornerBw:     return IRECT(w.R - kInsetX - kCornerW, ty, w.R - kInsetX, by);
      case kCornerLow:    return IRECT(w.L + kInsetX, cy + 1.f, w.L + kInsetX + kCornerW, w.B - kGap - 1.f);
      case kCornerHigh:   return IRECT(w.R - kInsetX - kCornerW, cy + 1.f, w.R - kInsetX, w.B - kGap - 1.f);
    }
    return IRECT();
  }

  float NormToX(float norm) const { const IRECT s = SliderRect(); return s.L + norm * s.W(); }
  float XToNorm(float x) const { const IRECT s = SliderRect(); return std::clamp((x - s.L) / s.W(), 0.f, 1.f); }

  float LowNorm() const
  {
    const IParam* pf = GetParam(0);
    const double centerHz = pf->FromNormalized(GetValue(0));
    const double bw = GetParam(1)->FromNormalized(GetValue(1));
    return (float) pf->ToNormalized(centerHz * std::pow(2., -bw / 2.));
  }

  float HighNorm() const
  {
    const IParam* pf = GetParam(0);
    const double centerHz = pf->FromNormalized(GetValue(0));
    const double bw = GetParam(1)->FromNormalized(GetValue(1));
    return (float) pf->ToNormalized(centerHz * std::pow(2., bw / 2.));
  }

  void NodeDrag(float x, float y)
  {
    const IRECT p = PlotRect();
    x = std::clamp(x, p.L, p.R);
    y = std::clamp(y, p.T, p.B);
    const float xn = (x - p.L) / p.W();
    const float yn = 1.f - (y - p.T) / p.H();
    SetValue((double) xn, 0);
    SetValue((double) yn, 1);
    SetDirty(true);
  }

  void DrawSlider(IGraphics& g)
  {
    const IRECT s = SliderRect();
    const float y = s.MH();
    const float lx = NormToX(LowNorm());
    const float hx = NormToX(HighNorm());
    // 外侧浅灰轨道
    g.FillRect(FP_TRACK, IRECT(s.L, y - 2.f, s.R, y + 2.f));
    // 两控制点之间涂黑
    g.FillRect(FP_BLACK, IRECT(lx, y - 2.f, hx, y + 2.f));
    // 控制点: 白底黑描边圆
    for (float px : { lx, hx })
    {
      g.FillCircle(FP_WHITE, px, y, 7.f);
      g.DrawCircle(FP_BLACK, px, y, 7.f, nullptr, 1.5f);
    }
  }

  void DrawCorner(IGraphics& g, int id)
  {
    const IRECT r = CornerRect(id);
    if (mOverCorner == id)
      g.FillRoundRect(FP_HOVER, r.GetPadded(-2.f), 4.f);
    WDL_String label; GetCornerLabel(id, label);
    const EAlign align = (id == kCornerBw || id == kCornerHigh) ? EAlign::Far : EAlign::Near;
    const IText t(11, FP_BLACK, "Outfit-SemiBold", align, EVAlign::Middle);
    g.DrawText(t, label.Get(), r);
  }

  void GetCornerLabel(int id, WDL_String& out) const
  {
    const IParam* pf = GetParam(0);
    const double c = pf->FromNormalized(GetValue(0));
    const double bw = GetParam(1)->FromNormalized(GetValue(1));
    char buf[32];
    switch (id)
    {
      case kCornerCenter: FormatFreq(buf, 32, c);                       out.SetFormatted(64, "CENTER %s", buf); break;
      case kCornerBw:     std::snprintf(buf, 32, "%.2f", bw);            out.SetFormatted(64, "BANDWIDTH %s", buf); break;
      case kCornerLow:    FormatFreq(buf, 32, c * std::pow(2., -bw/2.)); out.SetFormatted(64, "LOWCUT %s", buf); break;
      case kCornerHigh:   FormatFreq(buf, 32, c * std::pow(2.,  bw/2.)); out.SetFormatted(64, "HIGHCUT %s", buf); break;
    }
  }

  static void FormatFreq(char* b, int n, double hz)
  {
    if (hz >= 10000.) std::snprintf(b, n, "%.1fk", hz / 1000.);
    else if (hz >= 1000.) std::snprintf(b, n, "%.2fk", hz / 1000.);
    else std::snprintf(b, n, "%.0f", hz);
  }

  static bool ParseFreq(const char* s, double& hz)
  {
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (end == s) return false;
    while (*end && std::isspace((unsigned char)*end)) ++end;
    if (*end == 'k' || *end == 'K') hz = v * 1000.;
    else hz = v;
    return true;
  }

  static constexpr float kInsetX  = 8.f;
  static constexpr float kCornerW = 150.f;
  static constexpr float kCornerH = 15.f;
  static constexpr float kTopPad  = 18.f;
  static constexpr float kCutRowH = 16.f;
  static constexpr float kSliderH = 20.f;
  static constexpr float kGap     = 4.f;
  static constexpr float kMinGap  = 0.01f; // 两控制点最小归一化间隔 (~2/3 半音)

  Hooks mHooks;
  int   mOverCorner = -1;
  int   mActiveHandle = -1; // 0=左(low) 1=右(high) 2=中间(整体平移)
  float mStartX = 0.f, mStartLow = 0.f, mStartHigh = 1.f;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
