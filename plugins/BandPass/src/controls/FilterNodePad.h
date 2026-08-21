#pragma once

#include "IControls.h"
#include "../Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class FilterNodePad : public IVXYPadControl
{
public:
  struct Hooks
  {
    std::function<void(int cornerId, double value)> editCorner;
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
    DrawCorner(g, kCornerCenter);
    DrawCorner(g, kCornerBw);
    DrawSideLabel(g);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    mOverCorner = -1;
    for (int id : { kCornerCenter, kCornerBw })
    {
      if (CornerRect(id).Contains(x, y))
      {
        WDL_String init; GetCornerLabel(id, init);
        EAlign align = (id == kCornerBw) ? EAlign::Far : EAlign::Near;
        IText t(10, COL_BLACK, "Outfit-SemiBold", align, EVAlign::Middle);
        mEditingCorner = id;
        GetUI()->CreateTextEntry(*this, t, CornerRect(id), init.Get(), kNoValIdx);
        return;
      }
    }
    IVXYPadControl::OnMouseDown(x, y, mod);
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override
  {
    int hit = -1;
    for (int id : { kCornerCenter, kCornerBw })
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
    for (int id : { kCornerCenter, kCornerBw })
      if (CornerRect(id).Contains(x, y)) return true;
    if (SideLabelRect().Contains(x, y)) return true;
    return false;
  }

  void DrawSideLabel(IGraphics& g)
  {
    if (mSideLabel.GetLength() == 0) return;
    const IRECT r = SideLabelRect();
    IText t(10, COL_BLACK, "Outfit-SemiBold", EAlign::Center, EVAlign::Middle, -90.f);
    g.DrawText(t, mSideLabel.Get(), r);
  }

private:
  IRECT PlotRect() const
  {
    const IRECT& w = mWidgetBounds;
    const float top = w.T + kTopPad;
    return IRECT(w.L, top, w.R, w.B);
  }

  IRECT CornerRect(int id) const
  {
    const IRECT& w = mWidgetBounds;
    switch (id)
    {
      case kCornerCenter: return IRECT(w.L , w.T - kSideH, w.L + kCornerW, w.T + kCornerTextH);
      case kCornerBw:     return IRECT(w.R - kCornerW, w.T - kSideH, w.R, w.T + kCornerTextH);
    }
    return IRECT();
  }

  IRECT SideLabelRect() const
  {
    const IRECT& w = mWidgetBounds;
    return IRECT(w.L - kSideW, w.T, w.L, w.B);
  }

  void DrawCorner(IGraphics& g, int id)
  {
    const IRECT r = CornerRect(id);
    if (mOverCorner == id)
      g.FillRoundRect(COL_HOVER, r.GetPadded(-2.f), 4.f);
    WDL_String label; GetCornerLabel(id, label);
    const EAlign align = (id == kCornerBw) ? EAlign::Far : EAlign::Near;
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
      case kCornerCenter: FormatFreq(buf, 32, c);            out.SetFormatted(64, "CENTER %s", buf); break;
      case kCornerBw:     std::snprintf(buf, 32, "%.2f", bw); out.SetFormatted(64, "BANDWIDTH %s", buf); break;
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

  Hooks mHooks;
  WDL_String mSideLabel;
  int   mEditingCorner = -1;
  int   mOverCorner = -1;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
