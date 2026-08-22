#pragma once

#include "IControls.h"
#include "ISender.h"
#include "../Theme.h"

#include <algorithm>
#include <array>
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

  using TDataPacket = std::array<float, 4096>;

  enum MsgTags
  {
    kMsgTagSampleRate = 1,
    kMsgTagFFTSize,
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

  void OnMsgFromDelegate(int msgTag, int dataSize, const void* pData) override
  {
    IByteStream stream(pData, dataSize);

    if (msgTag == ISender<>::kUpdateMessage)
    {
      ISenderData<2, TDataPacket> d;
      stream.Get(&d, 0);
      const int nBins = std::min((int) d.vals[0].size(), std::max(mNumBins, 0));
      if (nBins <= 0) return;

      const double updatePeriod = (double) nBins * 2.0 / 4.0 / std::max(mSampleRate, 1.0);
      mAttackCoeff  = (float) std::exp(-updatePeriod / 0.003);
      mReleaseCoeff = (float) std::exp(-updatePeriod / 0.08);

      if (mSpectrumIn.size() != (size_t) nBins)
      {
        mSpectrumIn.assign(nBins, 0.f);
        mSpectrumOut.assign(nBins, 0.f);
      }
      const float a = mAttackCoeff, r = mReleaseCoeff;
      for (int i = 0; i < nBins; ++i)
      {
        const float raw = d.vals[0][i], prev = mSpectrumIn[i];
        const float coef = (raw > prev) ? a : r;
        mSpectrumIn[i] = coef * prev + (1.f - coef) * raw;
      }
      for (int i = 0; i < nBins; ++i)
      {
        const float raw = d.vals[1][i], prev = mSpectrumOut[i];
        const float coef = (raw > prev) ? a : r;
        mSpectrumOut[i] = coef * prev + (1.f - coef) * raw;
      }
      SetDirty(false);
    }
    else if (msgTag == kMsgTagSampleRate)
    {
      double sr;
      stream.Get(&sr, 0);
      mSampleRate = sr;
    }
    else if (msgTag == kMsgTagFFTSize)
    {
      int fftSize;
      stream.Get(&fftSize, 0);
      mNumBins = std::max(fftSize / 2, 1);
    }
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
    DrawSpectrum(g);
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

  void DrawSpectrum(IGraphics& g)
  {
    const IRECT tb = PlotRect();
    if (tb.W() <= 0.f || tb.H() <= 0.f) return;
    if (mSpectrumIn.empty() || mSpectrumOut.empty() || mNumBins <= 0) return;

    const IParam* pf = GetParam(0);
    const double binHz = mSampleRate / std::max((double) mNumBins * 2.0, 1.0);

    struct Pt { float x, y; };

    auto drawFill = [&](const std::vector<float>& spec, const IColor& topColor, const IColor& bottomColor)
    {
      // Logarithmic band aggregation: fold the linear FFT bins into a fixed
      // number of log-spaced bands (max per band keeps peaks), which removes
      // high-frequency raggedness and cuts the drawn point count ~8x.
      std::vector<Pt> pts;
      pts.reserve(kSpectrumBands);
      {
        const double logLo = std::log2(kSpecFreqLo);
        const double logHi = std::log2(kSpecFreqHi);
        const double logBand = (logHi - logLo) / kSpectrumBands;
        std::vector<float> bandMax(kSpectrumBands, 0.f);
        std::vector<char>  bandUsed(kSpectrumBands, 0);
        for (int i = 0; i < mNumBins && i < (int) spec.size(); ++i)
        {
          const double f = (double) i * binHz;
          if (f < kSpecFreqLo || f > kSpecFreqHi) continue;
          const int b = (int) ((std::log2(f) - logLo) / logBand);
          if (b < 0 || b >= kSpectrumBands) continue;
          const float amp = spec[i];
          if (amp > bandMax[b]) bandMax[b] = amp;
          bandUsed[b] = 1;
        }
        for (int b = 0; b < kSpectrumBands; ++b)
        {
          if (!bandUsed[b]) continue;
          const double fCenter = kSpecFreqLo * std::exp2(logBand * (b + 0.5));
          const float x = tb.L + (float) pf->ToNormalized(fCenter) * tb.W();
          const float amp = bandMax[b];
          const float db = (amp > 1e-6f) ? std::clamp(20.f * std::log10(amp), kSpectrumBottomDb, 0.f)
                                         : kSpectrumBottomDb;
          const float y = tb.B - (db - kSpectrumBottomDb) / (0.f - kSpectrumBottomDb) * tb.H();
          pts.push_back({ x, y });
        }
      }
      if (pts.size() < 2) return;

      g.PathClear();
      g.PathMoveTo(pts[0].x, pts[0].y);
      if (pts.size() > 3)
      {
        // Catmull-Rom to cubic Bezier, tension s in [0,1] (0 = straight lines)
        const float s = 0.6f;
        const int n = (int) pts.size();
        for (int i = 0; i < n - 1; ++i)
        {
          const Pt& p0 = pts[std::max(i - 1, 0)];
          const Pt& p1 = pts[i];
          const Pt& p2 = pts[i + 1];
          const Pt& p3 = pts[std::min(i + 2, n - 1)];
          const float c1x = p1.x + (p2.x - p0.x) * (s / 6.f);
          const float c1y = p1.y + (p2.y - p0.y) * (s / 6.f);
          const float c2x = p2.x - (p3.x - p1.x) * (s / 6.f);
          const float c2y = p2.y - (p3.y - p1.y) * (s / 6.f);
          g.PathCubicBezierTo(c1x, c1y, c2x, c2y, p2.x, p2.y);
        }
      }
      else
      {
        for (int i = 1; i < (int) pts.size(); ++i)
          g.PathLineTo(pts[i].x, pts[i].y);
      }
      g.PathLineTo(pts.back().x, tb.B);
      g.PathLineTo(pts[0].x, tb.B);
      g.PathClose();

      IPattern fill = IPattern::CreateLinearGradient(tb, EDirection::Vertical,
                       { IColorStop(topColor, 0.f), IColorStop(bottomColor, 1.f) });
      g.PathFill(fill);
    };

    drawFill(mSpectrumIn,  IColor(110, 153, 153, 153), IColor(0, 153, 153, 153)); // dry input (light grey)
    drawFill(mSpectrumOut, IColor(170,  56,  56,  56), IColor(0,  56,  56,  56)); // band-pass wet output (dark grey)
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

  // Bottom of the spectrum dB scale (top is 0 dBFS).
  static constexpr float kSpectrumBottomDb = -85.f;

  // Logarithmic band aggregation: the linear FFT bins are folded into this many
  // log-spaced display bands (max per band keeps peaks), removing high-frequency
  // raggedness and cutting the drawn point count ~8x.
  static constexpr int   kSpectrumBands = 256;
  static constexpr float kSpecFreqLo    = 20.f;
  static constexpr float kSpecFreqHi    = 20000.f;

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

  std::vector<float> mSpectrumIn;   // ch0: dry input spectrum, time-smoothed magnitudes
  std::vector<float> mSpectrumOut;  // ch1: band-pass wet output spectrum, time-smoothed
  // One-pole time-smoothing coefficients (attack fast / release slow), recomputed
  // from the sample rate and FFT size on every received packet.
  float mAttackCoeff  = 0.2f;
  float mReleaseCoeff = 0.9f;
  // Defaults match the sender (ISpectrumSender<2> with kSpectrumFFTSize=4096) so the
  // spectrum renders even if the config messages arrive before this control attaches.
  // OnIdle re-sends the real sample rate / FFT size every frame, so these converge
  // to the exact values shortly after the UI opens.
  int    mNumBins = 2048;           // FFT size / 2
  double mSampleRate = 48000.0;     // Hz
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
