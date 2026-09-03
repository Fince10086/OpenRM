#pragma once

#include "IControls.h"
#include "UiUtils.h"
#include "../Theme.h"

#include <algorithm>
#include <cmath>
#include <functional>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class BandRangeSlider : public IControl {
public:
  struct Hooks {
    std::function<void()> gestureBegin;
    std::function<void(int cornerId, double value)> editCorner;
    std::function<void(double lowNorm, double highNorm)> editBand;
  };

  BandRangeSlider(const IRECT &bounds, const std::initializer_list<int> &params, const Hooks &hooks)
      : IControl(bounds, params), mHooks(hooks) {
    SetTextEntryLength(20);
  }

  void SetLowPrefix(const char *s) {
    mLowPrefix.Set(s);
    SetDirty(false);
  }
  void SetHighPrefix(const char *s) {
    mHighPrefix.Set(s);
    SetDirty(false);
  }

  void SetGhost(bool ghost) {
    if (mGhost == ghost)
      return;
    mGhost = ghost;
    SetDirty(false);
  }

  void Draw(IGraphics &g) override {
    if (mGhost) {
      DrawTrackOnly(g);
      DrawGhostOverlay(g, mRECT);
      return;
    }
    DrawHeader(g);
    DrawBand(g);
  }

  void OnMouseDown(float x, float y, const IMouseMod &mod) override {
    if (mGhost)
      return;
    for (int id : {kCornerLow, kCornerHigh}) {
      if (CutValueRect(id).Contains(x, y)) {
        WDL_String init;
        GetCutValue(id, init, false);
        const EAlign align = (id == kCornerLow) ? EAlign::Near : EAlign::Far;
        const IText t(20, COL_900(), kFontSemiBold, align, EVAlign::Middle);
        mEditingCorner = id;
        GetUI()->CreateTextEntry(*this, t, CutValueRect(id), init.Get(), kNoValIdx);
        return;
      }
    }
    SelectHandle(x);
    if (mHooks.gestureBegin)
      mHooks.gestureBegin();
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod &mod) override {
    if (mActiveHandle == -1)
      return;
    if (mActiveHandle == 0 || mActiveHandle == 1) {
      float n = XToNorm(x);
      if (mActiveHandle == 0) {
        const float hN = HighNorm();
        n = std::clamp(n, 0.f, hN - kMinGap);
        mHooks.editBand((double)n, (double)hN);
      } else {
        const float lN = LowNorm();
        n = std::clamp(n, lN + kMinGap, 1.f);
        mHooks.editBand((double)lN, (double)n);
      }
    } else {
      const float srx = TrackRect().W();
      const float d = (x - mStartX) / srx;
      float lN = std::clamp(mStartLow + d, 0.f, 1.f - kMinGap);
      float hN = std::clamp(mStartHigh + d, lN + kMinGap, 1.f);
      if (hN - lN < kMinGap) {
        hN = lN + kMinGap;
        if (hN > 1.f) {
          hN = 1.f;
          lN = 1.f - kMinGap;
        }
      }
      mHooks.editBand((double)lN, (double)hN);
    }
    SyncFromParams();
  }

  void OnMouseUp(float x, float y, const IMouseMod &mod) override { mActiveHandle = -1; }

  void OnMouseDblClick(float x, float y, const IMouseMod &mod) override {}

  void OnTextEntryCompletion(const char *str, int valIdx) override {
    const int id = mEditingCorner;
    mEditingCorner = -1;
    if (id < 0)
      return;
    double hz;
    if (!ParseFreq(str, hz))
      return;
    if (mHooks.editCorner)
      mHooks.editCorner(id, hz);
  }

private:
  static constexpr float kHeaderH = 26.f;
  static constexpr float kMinGap = 0.01f;

  IRECT HeaderRect() const { return IRECT(mRECT.L, mRECT.T, mRECT.R, mRECT.T + kHeaderH); }

  IRECT TrackRect() const {
    return IRECT(mRECT.L + HANDLE_R + HANDLE_RING, mRECT.T + kHeaderH, mRECT.R - HANDLE_R - HANDLE_RING, mRECT.B)
        .GetMidVPadded(2.f);
  }

  float NormToX(float norm) const {
    const IRECT s = TrackRect();
    return s.L + norm * s.W();
  }
  float XToNorm(float x) const {
    const IRECT s = TrackRect();
    return std::clamp((x - s.L) / s.W(), 0.f, 1.f);
  }

  float LowNorm() const {
    const IParam *pf = GetParam(0);
    const double centerHz = pf->FromNormalized(GetValue(0));
    const double bw = GetParam(1)->FromNormalized(GetValue(1));
    return (float)pf->ToNormalized(centerHz / bw);
  }

  float HighNorm() const {
    const IParam *pf = GetParam(0);
    const double centerHz = pf->FromNormalized(GetValue(0));
    const double bw = GetParam(1)->FromNormalized(GetValue(1));
    return (float)pf->ToNormalized(centerHz * bw);
  }

  void SyncFromParams() {
    SetValue(GetParam(0)->GetNormalized(), 0);
    SetValue(GetParam(1)->GetNormalized(), 1);
    SetDirty(false);
  }

  void SelectHandle(float x) {
    const float lx = NormToX(LowNorm()), hx = NormToX(HighNorm());
    const float dL = std::fabs(x - lx), dR = std::fabs(x - hx);
    if (x > lx + 7.f && x < hx - 7.f) {
      mActiveHandle = 2;
      mStartX = x;
      mStartLow = LowNorm();
      mStartHigh = HighNorm();
    } else if (dL <= dR)
      mActiveHandle = 0;
    else
      mActiveHandle = 1;
  }

  void DrawHeader(IGraphics &g) {
    const IRECT hdr = HeaderRect();
    const IText t(20, COL_900(), kFontSemiBold, EAlign::Near, EVAlign::Middle);
    const IText tf(20, COL_900(), kFontSemiBold, EAlign::Far, EVAlign::Middle);

    WDL_String value;
    IRECT measured;
    GetCutValue(kCornerLow, value, true);
    g.MeasureText(t, mLowPrefix.Get(), measured);
    const float lowPrefixW = measured.W();
    g.MeasureText(t, value.Get(), measured);
    mCutValueRect[0] =
        IRECT(hdr.L + lowPrefixW + LABEL_VALUE_GAP, hdr.T, hdr.L + lowPrefixW + LABEL_VALUE_GAP + measured.W(), hdr.B);
    g.DrawText(t, mLowPrefix.Get(), hdr);
    g.DrawText(t, value.Get(), mCutValueRect[0]);

    GetCutValue(kCornerHigh, value, true);
    g.MeasureText(tf, value.Get(), measured);
    mCutValueRect[1] = IRECT(hdr.R - measured.W(), hdr.T, hdr.R, hdr.B);
    g.DrawText(tf, mHighPrefix.Get(), IRECT(hdr.L, hdr.T, mCutValueRect[1].L - LABEL_VALUE_GAP, hdr.B));
    g.DrawText(tf, value.Get(), mCutValueRect[1]);
  }

  void DrawTrackOnly(IGraphics &g) {
    const IRECT s = TrackRect();
    const float y = s.MH();
    g.FillRect(COL_300(), IRECT(mRECT.L, y - 2.f, mRECT.R, y + 2.f));
    g.FillRect(COL_500(), IRECT(NormToX(LowNorm()), y - 2.f, NormToX(HighNorm()), y + 2.f));
  }

  void DrawBand(IGraphics &g) {
    const IRECT s = TrackRect();
    const float y = s.MH();
    const float lx = NormToX(LowNorm());
    const float hx = NormToX(HighNorm());
    g.FillRect(COL_300(), IRECT(mRECT.L, y - 2.f, mRECT.R, y + 2.f));
    g.FillRect(COL_500(), IRECT(lx, y - 2.f, hx, y + 2.f));
    for (float px : {lx, hx})
      DrawKnob(g, px, y);
  }

  IRECT CutValueRect(int id) const { return mCutValueRect[id == kCornerHigh]; }

  void GetCutValue(int id, WDL_String &out, bool withUnit) const {
    // 显示未调制的目标值; FilterNodePad 显示叠加随机调制后的实时值
    const IParam *pf = GetParam(0);
    const double c = pf->FromNormalized(GetValue(0));
    const double bw = GetParam(1)->FromNormalized(GetValue(1));
    double hz = (id == kCornerLow) ? c / bw : c * bw;
    if (id == kCornerLow)
      hz = std::max(hz, 20.);
    else
      hz = std::min(hz, 20500.);
    char buf[32];
    FormatFreq(buf, 32, hz, withUnit);
    out.Set(buf);
  }

  Hooks mHooks;
  WDL_String mLowPrefix{"LOWCUT"};
  WDL_String mHighPrefix{"HIGHCUT"};
  IRECT mCutValueRect[2];
  bool mGhost = false;
  int mEditingCorner = -1;
  int mActiveHandle = -1;
  float mStartX = 0.f, mStartLow = 0.f, mStartHigh = 1.f;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
