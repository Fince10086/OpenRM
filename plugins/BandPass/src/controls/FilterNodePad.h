#pragma once

#include "IControls.h"
#include "../Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

enum EPadCorner : int
{
  kCornerCenter = 4,
  kCornerBw     = 5,
  kCornerLow    = 6,
  kCornerHigh   = 7
};

class FilterNodePad : public IVXYPadControl
{
public:
  struct Hooks
  {
    std::function<void()> gestureBegin;
    std::function<void(int cornerId, double value)> editCorner;
    std::function<void(double lowNorm, double highNorm)> editBand;
  };

  FilterNodePad(const IRECT& bounds, const std::initializer_list<int>& params,
                const char* label, const IVStyle& style, const Hooks& hooks,
                float handleRadius = 9.f)
  : IVXYPadControl(bounds, params,  "", style.WithDrawFrame(false), handleRadius, true, true)
  , mHooks(hooks)
  , mSideLabel(label)
  {
    SetTextEntryLength(20);
  }

  void Draw(IGraphics& g) override
  {
    IVXYPadControl::Draw(g);
    DrawSlider(g);
    DrawCorner(g, kCornerCenter);
    DrawCorner(g, kCornerBw);
    DrawCorner(g, kCornerLow);
    DrawCorner(g, kCornerHigh);
    DrawSideLabel(g);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    mOverCorner = -1;
    for (int id : { kCornerCenter, kCornerBw, kCornerLow, kCornerHigh })
    {
      if (CornerRect(id).Contains(x, y))
      {
        WDL_String init; GetCornerLabel(id, init);
        EAlign align = (id == kCornerBw || id == kCornerHigh) ? EAlign::Far : EAlign::Near;
        IText t(10, COL_BLACK, "Outfit-SemiBold", align, EVAlign::Middle);
        mEditingCorner = id;
        GetUI()->CreateTextEntry(*this, t, CornerRect(id), init.Get(), kNoValIdx);
        return;
      }
    }
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
    if (PlotRect().Contains(x, y))
    {
      IVXYPadControl::OnMouseDown(x, y, mod);
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
    const int id = mEditingCorner;
    mEditingCorner = -1;
    if (id < 0) return;
    double v;
    if (id == kCornerBw) { char* end = nullptr; v = std::strtod(str, &end); if (end == str) return; }
    else if (!ParseFreq(str, v)) return;
    if (mHooks.editCorner) mHooks.editCorner(id, v);
  }

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

  void DrawTrack(IGraphics& g) override
  {
    const IRECT tb = PlotRect();
    for (int i = 1; i < 4; ++i)
    {
      const float y = tb.T + tb.H() * i / 4.f;
      g.DrawLine(COL_GRID, tb.L, y, tb.R, y, nullptr, 1.f);
      const float x = tb.L + tb.W() * i / 4.f;
      g.DrawLine(COL_GRID, x, tb.T, x, tb.B, nullptr, 1.f);
    }
    g.DrawRect(COL_BLACK, tb, nullptr, 1.f);
  }

  void DrawHandle(IGraphics& g, const IRECT& trackBounds, const IRECT& handleBounds) override
  {
    const float cx = handleBounds.MW();
    const float cy = handleBounds.MH();
    const float r  = handleBounds.W() * 0.5f;
    g.DrawLine(COL_BLACK, cx, handleBounds.T, cx, cy - r, nullptr, 1.f);
    g.FillCircle(COL_BLACK, cx, cy, r);
    g.FillCircle(COLOR_WHITE, cx, cy, r * 0.25f);
  }

  bool IsHit(float x, float y) const override
  {
    if (mTargetRECT.Contains(x, y)) return true;
    for (int id : { kCornerCenter, kCornerBw, kCornerLow, kCornerHigh })
      if (CornerRect(id).Contains(x, y)) return true;
    if (SideLabelRect().Contains(x, y)) return true;
    return false;
  }

  void DrawSideLabel(IGraphics& g)
  {
    if (mSideLabel.GetLength() == 0) return;
    const IRECT r = SideLabelRect();
    IText t(11, COL_BLACK, "Outfit-SemiBold", EAlign::Center, EVAlign::Middle, -90.f);
    g.DrawText(t, mSideLabel.Get(), r);
  }

private:
  IRECT PlotRect() const
  {
    const IRECT& w = mWidgetBounds;
    const float top = w.T + kTopPad;
    const float srTop = w.B - kBottomInset - kSliderH;
    return IRECT(w.L, top, w.R, srTop);
  }

  IRECT SliderRect() const
  {
    const IRECT& w = mWidgetBounds;
    const float top = w.B - kBottomInset - kSliderH;
    return IRECT(w.L + 2.f, top, w.R - 2.f, top + kSliderH);
  }

  IRECT CornerRect(int id) const
  {
    const IRECT& w = mWidgetBounds;
    switch (id)
    {
      case kCornerCenter: return IRECT(w.L , w.T - kSideH, w.L + kCornerW, w.T + kCornerTextH);
      case kCornerBw:     return IRECT(w.R - kCornerW, w.T - kSideH, w.R, w.T + kCornerTextH);
      case kCornerLow:    return IRECT(w.L , w.B - kCornerTextH, w.L + kCornerW, w.B + kSideH);
      case kCornerHigh:   return IRECT(w.R - kCornerW, w.B - kCornerTextH, w.R, w.B + kSideH);
    }
    return IRECT();
  }

  IRECT SideLabelRect() const
  {
    const IRECT& w = mWidgetBounds;
    return IRECT(w.L - kSideW, w.T, w.L, w.B);
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
    g.FillRect(COL_TRACK, IRECT(s.L, y - 2.f, s.R, y + 2.f));
    g.FillRect(COL_BLACK, IRECT(lx, y - 2.f, hx, y + 2.f));
    for (float px : { lx, hx })
    {
      g.FillCircle(COLOR_WHITE, px, y, 7.f);
      g.DrawCircle(COL_BLACK, px, y, 7.f, nullptr, 1.5f);
    }
  }

  void DrawCorner(IGraphics& g, int id)
  {
    const IRECT r = CornerRect(id);
    if (mOverCorner == id)
      g.FillRoundRect(COL_HOVER, r.GetPadded(-2.f), 4.f);
    WDL_String label; GetCornerLabel(id, label);
    const EAlign align = (id == kCornerBw || id == kCornerHigh) ? EAlign::Far : EAlign::Near;
    const IText t(10, COL_BLACK, "Outfit-SemiBold", align, EVAlign::Middle);
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

  static constexpr float kCornerW = 80.f;
  static constexpr float kCornerTextH = 10.f;
  static constexpr float kSideW    = 22.f;
  static constexpr float kSideH    = 0.f;
  static constexpr float kTopPad   = 18.f;
  static constexpr float kSliderH  = 20.f;
  static constexpr float kBottomInset = 2.f;
  static constexpr float kMinGap   = 0.01f;

  Hooks mHooks;
  WDL_String mSideLabel;
  int   mEditingCorner = -1;
  int   mOverCorner = -1;
  int   mActiveHandle = -1;
  float mStartX = 0.f, mStartLow = 0.f, mStartHigh = 1.f;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
