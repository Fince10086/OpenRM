#pragma once

#include "IControls.h"
#include "../Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
    std::function<void(int slopeDb)> editSlope;
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

  void SetSideLabel(const char* s) { mSideLabel.Set(s); SetDirty(false); }
  void SetCenterPrefix(const char* s) { mCenterPrefix.Set(s); SetDirty(false); }
  void SetBwPrefix(const char* s) { mBwPrefix.Set(s); SetDirty(false); }
  void SetSlopePrefix(const char* s) { mSlopePrefix.Set(s); SetDirty(false); }
  void SetSlopeIndex(int idx) { mSlopeIndex = idx; SetDirty(false); }

  void Draw(IGraphics& g) override
  {
    IVXYPadControl::Draw(g);
    DrawCorner(g, kCornerCenter);
    DrawCorner(g, kCornerBw);
    DrawSlope(g);
    DrawSideLabel(g);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    if (mSlopeRect.Contains(x, y))
    {
      OpenSlopeMenu();
      return;
    }
    for (int id : { kCornerCenter, kCornerBw })
    {
      if (CornerValueRect(id).Contains(x, y))
      {
        WDL_String init; GetCornerValue(id, init, false);
        EAlign align = (id == kCornerBw) ? EAlign::Far : EAlign::Near;
        IText t(20, COL_BLACK, FontSemiBold(), align, EVAlign::Middle);
        mEditingCorner = id;
        GetUI()->CreateTextEntry(*this, t, CornerValueRect(id), init.Get(), kNoValIdx);
        return;
      }
    }
    IVXYPadControl::OnMouseDown(x, y, mod);
  }

  // Don't inherit the base-class double-click reset-to-default behavior.
  void OnMouseDblClick(float x, float y, const IMouseMod& mod) override {}

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
    const IParam* pf = GetParam(0);
    auto xOf = [&](double f) { return tb.L + (float) pf->ToNormalized(f) * tb.W(); };

    struct Band { double lo, hi; float v0, v1; };
    static const Band kBands[] = {
      { 20.,    100.,   194.f, 218.f },
      { 100.,   1000.,  205.f, 229.f },
      { 1000.,  10000., 216.f, 240.f },
      { 10000., 20000., 227.f, 227.f },
    };

    for (const Band& band : kBands)
    {
      double edges[16]; int n = 0;
      edges[n++] = band.lo;
      for (int decade = 1; decade <= 10000; decade *= 10)
        for (int m = 1; m <= 9; ++m)
        {
          const double f = m * decade;
          if (f > band.lo && f < band.hi) edges[n++] = f;
        }
      edges[n++] = band.hi;

      float xs[16];
      for (int i = 0; i < n; ++i) xs[i] = xOf(edges[i]);

      const int cells = n - 1;
      for (int i = 0; i < cells; ++i)
      {
        const float t = (cells > 1) ? (float) i / (cells - 1) : 0.f;
        const int v = (int) std::lround(band.v0 + (band.v1 - band.v0) * t);
        g.FillRect(IColor(255, v, v, v), IRECT(xs[i], tb.T, xs[i + 1], tb.B));
      }
    }
  }

  void DrawHandle(IGraphics& g, const IRECT&, const IRECT& handleBounds) override
  {
    const float cx = handleBounds.MW();
    const float cy = handleBounds.MH();
    g.FillCircle(COL_ACCENT, cx, cy, mHandleRadius);
    g.FillCircle(COLOR_WHITE, cx, cy, mHandleRadius * 0.25f);
  }

  bool IsHit(float x, float y) const override
  {
    if (mTargetRECT.Contains(x, y)) return true;
    for (int id : { kCornerCenter, kCornerBw })
      if (CornerRect(id).Contains(x, y)) return true;
    if (mSlopeRect.Contains(x, y)) return true;
    if (SideLabelRect().Contains(x, y)) return true;
    return false;
  }

  void DrawSideLabel(IGraphics& g)
  {
    if (mSideLabel.GetLength() == 0) return;
    const IRECT r = SideLabelRect();
    IText t(28, COL_HOVER, FontBold(), EAlign::Center, EVAlign::Middle, -90.f);
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
    // Hug the inner-left edge of the plot area, centred vertically within it
    // (not within the whole control, which includes the corner-label strip).
    return IRECT(w.L, w.T + kTopPad, w.L + kSideW, w.B);
  }

  void DrawCorner(IGraphics& g, int id)
  {
    const IRECT r = CornerRect(id);
    const bool far = (id == kCornerBw);
    const EAlign align = far ? EAlign::Far : EAlign::Near;
    const IText t(20, COL_BLACK, FontSemiBold(), align, EVAlign::Middle);
    const char* prefix = far ? mBwPrefix.Get() : mCenterPrefix.Get();
    WDL_String value;
    GetCornerValue(id, value, true);
    IRECT measured;
    if (far)
    {
      g.MeasureText(t, value.Get(), measured);
      mCornerValueRect[1] = IRECT(r.R - measured.W(), r.T, r.R, r.B);
      g.DrawText(t, prefix, IRECT(r.L, r.T, mCornerValueRect[1].L - LABEL_VALUE_GAP, r.B));
      g.DrawText(t, value.Get(), mCornerValueRect[1]);
    }
    else
    {
      g.MeasureText(t, prefix, measured);
      const float prefixW = measured.W();
      g.MeasureText(t, value.Get(), measured);
      mCornerValueRect[0] = IRECT(r.L + prefixW + LABEL_VALUE_GAP, r.T,
                                  r.L + prefixW + LABEL_VALUE_GAP + measured.W(), r.B);
      g.DrawText(t, prefix, r);
      g.DrawText(t, value.Get(), mCornerValueRect[0]);
    }
  }

  IRECT CornerValueRect(int id) const { return mCornerValueRect[id == kCornerBw]; }

  // ---- SLOPE dropdown (centred between CENTER and BANDWIDTH) ----

  IRECT SlopeRect() const
  {
    const IRECT& w = mWidgetBounds;
    return IRECT(w.L + kCornerW, w.T - kSideH, w.R - kCornerW, w.T + kCornerTextH);
  }

  void DrawSlope(IGraphics& g)
  {
    const IRECT r = SlopeRect();
    const IText t(20, COL_BLACK, FontSemiBold(), EAlign::Center, EVAlign::Middle);

    WDL_String value; GetSlopeValue(value);
    IRECT m1, m2;
    g.MeasureText(t, mSlopePrefix.Get(), m1);
    g.MeasureText(t, value.Get(), m2);
    const float totalW = m1.W() + LABEL_VALUE_GAP + m2.W();
    const float x0 = r.MW() - totalW * 0.5f;
    mSlopeRect = IRECT(x0, r.T, x0 + totalW, r.B);
    g.DrawText(t, mSlopePrefix.Get(), IRECT(x0, r.T, x0 + m1.W(), r.B));
    g.DrawText(t, value.Get(), IRECT(x0 + m1.W() + LABEL_VALUE_GAP, r.T, x0 + totalW, r.B));
  }

  void GetSlopeValue(WDL_String& out) const
  {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d dB/oct", kSlopeDb[std::clamp(mSlopeIndex, 0, 3)]);
    out.Set(buf);
  }

  void OpenSlopeMenu()
  {
    if (!GetUI()) return;
    mSlopeMenu.Clear();
    mSlopeMenu.SetFunction([this](IPopupMenu* menu) {
      const int idx = menu ? menu->GetChosenItemIdx() : -1;
      if (idx < 0 || idx >= 4) return;
      if (mHooks.editSlope) mHooks.editSlope(kSlopeDb[idx]);
    });
    for (int i = 0; i < 4; ++i)
    {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%d dB/oct", kSlopeDb[i]);
      mSlopeMenu.AddItem(buf);
    }
    mSlopeMenu.CheckItemAlone(std::clamp(mSlopeIndex, 0, 3));
    GetUI()->CreatePopupMenu(*this, mSlopeMenu, mSlopeRect, kNoValIdx);
  }

  void GetCornerValue(int id, WDL_String& out, bool withUnit) const
  {
    char buf[32];
    if (id == kCornerBw)
    {
      const double bw = GetParam(1)->FromNormalized(GetValue(1));
      std::snprintf(buf, 32, "%.2f", bw);
    }
    else
    {
      const IParam* pf = GetParam(0);
      const double c = pf->FromNormalized(GetValue(0));
      FormatFreq(buf, 32, c, withUnit);
    }
    out.Set(buf);
  }

  static void FormatFreq(char* b, int n, double hz, bool withUnit)
  {
    const char* u = withUnit ? "Hz" : "";
    if (hz >= 10000.) std::snprintf(b, n, "%.1fk%s", hz / 1000., u);
    else if (hz >= 1000.) std::snprintf(b, n, "%.2fk%s", hz / 1000., u);
    else std::snprintf(b, n, "%.0f%s", hz, u);
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

  static constexpr float kCornerW = 170.f;
  static constexpr float kCornerTextH = 22.f;
  static constexpr float kSideW    = 24.f;
  static constexpr float kSideH    = 0.f;
  static constexpr float kTopPad   = 30.f;

  Hooks mHooks;
  WDL_String mSideLabel;
  WDL_String mCenterPrefix { "CENTER" };
  WDL_String mBwPrefix { "BANDWIDTH" };
  WDL_String mSlopePrefix { "SLOPE" };
  IRECT mCornerValueRect[2];
  IRECT mSlopeRect;
  IPopupMenu mSlopeMenu;
  int   mEditingCorner = -1;
  int   mSlopeIndex = kSlopeDefaultIdx;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
