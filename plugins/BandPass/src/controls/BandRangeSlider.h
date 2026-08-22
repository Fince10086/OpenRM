#pragma once

#include "IControls.h"
#include "../Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

// Low/high cut range slider for one channel. Driven by the same center /
// bandwidth params as the XY pad above it. The two cut frequencies are drawn
// in a header strip above the track, like the labels of the other sliders:
// low cut on the top-left, high cut on the top-right. Clicking either text
// opens a text entry.
class BandRangeSlider : public IControl
{
public:
  struct Hooks
  {
    std::function<void()> gestureBegin;
    std::function<void(int cornerId, double value)> editCorner;
    std::function<void(double lowNorm, double highNorm)> editBand;
  };

  BandRangeSlider(const IRECT& bounds, const std::initializer_list<int>& params,
                  const Hooks& hooks)
  : IControl(bounds, params)
  , mHooks(hooks)
  {
    SetTextEntryLength(20);
  }

  void Draw(IGraphics& g) override
  {
    DrawHeader(g);
    DrawBand(g);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    if (HeaderRect().Contains(x, y))
    {
      const int id = x < HeaderRect().MW() ? kCornerLow : kCornerHigh;
      WDL_String init; GetCutLabel(id, init);
      const EAlign align = (id == kCornerLow) ? EAlign::Near : EAlign::Far;
      const IText t(20, COL_BLACK, "Outfit-SemiBold", align, EVAlign::Middle);
      mEditingCorner = id;
      GetUI()->CreateTextEntry(*this, t, HeaderRect(), init.Get(), kNoValIdx);
      return;
    }
    SelectHandle(x);
    if (mHooks.gestureBegin) mHooks.gestureBegin();
    // No OnMouseDrag here: a plain click must only grab the nearest handle,
    // not snap it to the click position (which would widen the band).
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override
  {
    if (mActiveHandle == -1) return;
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
    else
    {
      const float srx = TrackRect().W();
      const float d = (x - mStartX) / srx;
      float lN = std::clamp(mStartLow + d, 0.f, 1.f - kMinGap);
      float hN = std::clamp(mStartHigh + d, lN + kMinGap, 1.f);
      if (hN - lN < kMinGap) { hN = lN + kMinGap; if (hN > 1.f) { hN = 1.f; lN = 1.f - kMinGap; } }
      mHooks.editBand((double) lN, (double) hN);
    }
    // While dragging this control it is mouse-captured, so SetValueFromDelegate
    // from UpdatePads() is a no-op; sync our own values from the params instead.
    SyncFromParams();
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    mActiveHandle = -1;
  }

  // Don't inherit the base-class double-click reset-to-default behavior.
  void OnMouseDblClick(float x, float y, const IMouseMod& mod) override {}

  void OnTextEntryCompletion(const char* str, int valIdx) override
  {
    const int id = mEditingCorner;
    mEditingCorner = -1;
    if (id < 0) return;
    double hz;
    if (!ParseFreq(str, hz)) return;
    if (mHooks.editCorner) mHooks.editCorner(id, hz);
  }

private:
  static constexpr float kHeaderH = 26.f;
  static constexpr float kMinGap   = 0.01f;

  IRECT HeaderRect() const
  {
    return IRECT(mRECT.L, mRECT.T, mRECT.R, mRECT.T + kHeaderH);
  }

  IRECT TrackRect() const
  {
    return IRECT(mRECT.L + HANDLE_R + HANDLE_RING, mRECT.T + kHeaderH,
                 mRECT.R - HANDLE_R - HANDLE_RING, mRECT.B).GetMidVPadded(2.f);
  }

  float NormToX(float norm) const { const IRECT s = TrackRect(); return s.L + norm * s.W(); }
  float XToNorm(float x) const { const IRECT s = TrackRect(); return std::clamp((x - s.L) / s.W(), 0.f, 1.f); }

  float LowNorm() const
  {
    const IParam* pf = GetParam(0);
    const double centerHz = pf->FromNormalized(GetValue(0));
    const double bw = GetParam(1)->FromNormalized(GetValue(1));
    return (float) pf->ToNormalized(centerHz / bw);
  }

  float HighNorm() const
  {
    const IParam* pf = GetParam(0);
    const double centerHz = pf->FromNormalized(GetValue(0));
    const double bw = GetParam(1)->FromNormalized(GetValue(1));
    return (float) pf->ToNormalized(centerHz * bw);
  }

  void SyncFromParams()
  {
    SetValue(GetParam(0)->GetNormalized(), 0);
    SetValue(GetParam(1)->GetNormalized(), 1);
    SetDirty(false);
  }

  void SelectHandle(float x)
  {
    const float lx = NormToX(LowNorm()), hx = NormToX(HighNorm());
    const float dL = std::fabs(x - lx), dR = std::fabs(x - hx);
    if (x > lx + 7.f && x < hx - 7.f) { mActiveHandle = 2; mStartX = x; mStartLow = LowNorm(); mStartHigh = HighNorm(); }
    else if (dL <= dR) mActiveHandle = 0;
    else mActiveHandle = 1;
  }

  void DrawHeader(IGraphics& g)
  {
    WDL_String low, high;
    GetCutLabel(kCornerLow, low);
    GetCutLabel(kCornerHigh, high);
    const IRECT hdr = HeaderRect();
    g.DrawText(IText(20, COL_BLACK, "Outfit-SemiBold", EAlign::Near, EVAlign::Middle),
               low.Get(), IRECT(hdr.L, hdr.T, hdr.MW(), hdr.B));
    g.DrawText(IText(20, COL_BLACK, "Outfit-SemiBold", EAlign::Far, EVAlign::Middle),
               high.Get(), IRECT(hdr.MW(), hdr.T, hdr.R, hdr.B));
  }

  void DrawBand(IGraphics& g)
  {
    const IRECT s = TrackRect();
    const float y = s.MH();
    const float lx = NormToX(LowNorm());
    const float hx = NormToX(HighNorm());
    g.FillRect(COL_TRACK, IRECT(mRECT.L, y - 2.f, mRECT.R, y + 2.f));
    g.FillRect(COL_HOVER, IRECT(lx, y - 2.f, hx, y + 2.f));
    for (float px : { lx, hx })
    {
      g.FillCircle(COL_BG, px, y, HANDLE_R + HANDLE_RING);
      g.FillCircle(COL_ACCENT, px, y, HANDLE_R);
    }
  }

  void GetCutLabel(int id, WDL_String& out) const
  {
    const IParam* pf = GetParam(0);
    const double c = pf->FromNormalized(GetValue(0));
    const double bw = GetParam(1)->FromNormalized(GetValue(1));
    char buf[32];
    if (id == kCornerLow) FormatFreq(buf, 32, c / bw);
    else                  FormatFreq(buf, 32, c * bw);
    out.SetFormatted(64, "%s%s", id == kCornerLow ? "LOWCUT " : "HIGHCUT ", buf);
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

  Hooks mHooks;
  int   mEditingCorner = -1;
  int   mActiveHandle = -1;
  float mStartX = 0.f, mStartLow = 0.f, mStartHigh = 1.f;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
