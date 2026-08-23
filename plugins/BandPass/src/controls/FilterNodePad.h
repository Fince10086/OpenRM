#pragma once

#include "IControls.h"
#include "ISender.h"
#include "UiUtils.h"
#include "../Theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class FilterNodePad : public IVXYPadControl {
public:
  struct Hooks {
    std::function<void(int cornerId, double value)> editCorner;
    std::function<void(int slopeDb)> editSlope;
    std::function<void(int cornerId)> randomToggle;
    std::function<void(int cornerId, int colorIdx)> randomSetColor;
    std::function<void()> togglePass;
  };

  using TDataPacket = std::array<float, 4096>;

  enum MsgTags {
    kMsgTagSampleRate = 1,
    kMsgTagFFTSize,
  };

  FilterNodePad(const IRECT &bounds, const std::initializer_list<int> &params, const char *label, const IVStyle &style,
                const Hooks &hooks, float handleRadius = 9.f)
      : IVXYPadControl(bounds, params, "", style.WithDrawFrame(false), handleRadius, true, true), mHooks(hooks),
        mSideLabel(label) {
    SetTextEntryLength(20);
    // 预分配频谱绘制缓冲, 避免每帧 Draw 时动态分配
    mBandMax.assign(kSpectrumBands, 0.f);
    mBandUsed.assign(kSpectrumBands, 0);
    mSpecPts.reserve(kSpectrumBands);
  }

  void OnMsgFromDelegate(int msgTag, int dataSize, const void *pData) override {
    IByteStream stream(pData, dataSize);

    if (msgTag == ISender<>::kUpdateMessage) {
      ISenderData<2, TDataPacket> d;
      stream.Get(&d, 0);
      const int nBins = std::min((int)d.vals[0].size(), std::max(mNumBins, 0));
      if (nBins <= 0)
        return;

      const double updatePeriod = (double)nBins * 2.0 / 4.0 / std::max(mSampleRate, 1.0);
      mAttackCoeff = (float)std::exp(-updatePeriod / 0.003);
      mReleaseCoeff = (float)std::exp(-updatePeriod / 0.08);

      if (mSpectrumIn.size() != (size_t)nBins) {
        mSpectrumIn.assign(nBins, 0.f);
        mSpectrumOut.assign(nBins, 0.f);
      }
      const float a = mAttackCoeff, r = mReleaseCoeff;
      for (int i = 0; i < nBins; ++i) {
        const float raw = d.vals[0][i], prev = mSpectrumIn[i];
        const float coef = (raw > prev) ? a : r;
        mSpectrumIn[i] = coef * prev + (1.f - coef) * raw;
      }
      for (int i = 0; i < nBins; ++i) {
        const float raw = d.vals[1][i], prev = mSpectrumOut[i];
        const float coef = (raw > prev) ? a : r;
        mSpectrumOut[i] = coef * prev + (1.f - coef) * raw;
      }
      SetDirty(false);
    } else if (msgTag == kMsgTagSampleRate) {
      double sr;
      stream.Get(&sr, 0);
      mSampleRate = sr;
    } else if (msgTag == kMsgTagFFTSize) {
      int fftSize;
      stream.Get(&fftSize, 0);
      mNumBins = std::max(fftSize / 2, 1);
    }
  }

  void SetSideLabel(const char *s) {
    mSideLabel.Set(s);
    SetDirty(false);
  }

  void SetPass(bool pass) {
    mPass = pass;
    SetDirty(false);
  }

  IRECT PassRejectRect() const { return mPassRejectRect; }

  void SetGhost(bool ghost) {
    if (mGhost == ghost)
      return;
    mGhost = ghost;
    SetDirty(false);
  }
  void SetCenterPrefix(const char *s) {
    mCenterPrefix.Set(s);
    SetDirty(false);
  }
  void SetBwPrefix(const char *s) {
    mBwPrefix.Set(s);
    SetDirty(false);
  }
  void SetSlopePrefix(const char *s) {
    mSlopePrefix.Set(s);
    SetDirty(false);
  }
  void SetSlopeIndex(int idx) {
    mSlopeIndex = idx;
    SetDirty(false);
  }
  void SetRandomMap(bool centerOn, int centerColor, bool bwOn, int bwColor) {
    mRandomMapOn[0] = centerOn;
    mRandomMapColor[0] = centerColor;
    mRandomMapOn[1] = bwOn;
    mRandomMapColor[1] = bwColor;
    SetDirty(false);
  }
  void SetRandomDeltas(float freqOct, float bwOct) {
    if (std::fabs(freqOct - mRandomFreqOct) < 1e-4f && std::fabs(bwOct - mRandomBwOct) < 1e-4f)
      return;
    mRandomFreqOct = freqOct;
    mRandomBwOct = bwOct;
    SetDirty(false);
  }

  void Draw(IGraphics &g) override {
    g.FillRect(COL_100(), mRECT);
    if (mGhost) {
      DrawTrack(g);
      DrawGhostOverlay(g, mRECT);
      return;
    }
    DrawWidget(g);
    DrawCorner(g, kCornerCenter);
    DrawCorner(g, kCornerBw);
    DrawSlope(g);
    DrawPassToggle(g);
    DrawSideLabel(g);
  }

  void OnMouseOver(float x, float y, const IMouseMod &mod) override {
    const bool over = mPassRejectRect.Contains(x, y);
    if (over != mPassBtnHover) {
      mPassBtnHover = over;
      SetDirty(false);
    }
    IVXYPadControl::OnMouseOver(x, y, mod);
  }

  void OnMouseOut() override {
    if (mPassBtnHover) {
      mPassBtnHover = false;
      SetDirty(false);
    }
    IVXYPadControl::OnMouseOut();
  }

  void OnMouseDown(float x, float y, const IMouseMod &mod) override {
    if (mGhost)
      return;
    if (PassRejectRect().Contains(x, y)) {
      if (!mod.R && mHooks.togglePass)
        mHooks.togglePass();
      return;
    }
    if (mSlopeRect.Contains(x, y)) {
      if (!mod.R)
        OpenSlopeMenu();
      return;
    }
    for (int i = 0; i < 2; ++i) {
      if (mRandomSwatchRect[i].Contains(x, y)) {
        if (mod.R)
          OpenRandomColorMenu(i);
        else if (mHooks.randomToggle)
          mHooks.randomToggle(i ? kCornerBw : kCornerCenter);
        return;
      }
    }
    if (!mod.R) {
      for (int id : {kCornerCenter, kCornerBw}) {
        if (CornerValueRect(id).Contains(x, y)) {
          WDL_String init;
          GetCornerValue(id, init, false, false);
          EAlign align = (id == kCornerBw) ? EAlign::Far : EAlign::Near;
          IText t(20, COL_900(), kFontSemiBold, align, EVAlign::Middle);
          mEditingCorner = id;
          GetUI()->CreateTextEntry(*this, t, CornerValueRect(id), init.Get(), kNoValIdx);
          return;
        }
      }
      if (PlotRect().Contains(x, y))
        IVXYPadControl::OnMouseDown(x, y, mod);
    }
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod &mod) override {
    if (!mMouseDown)
      return;
    const IRECT tb = PlotRect();
    x = std::clamp(x, tb.L, tb.R);
    y = std::clamp(y, tb.T, tb.B);
    const float xn = (x - tb.L) / tb.W();
    const float yn = 1.f - ((y - tb.T) / tb.H());
    SetValue(xn, 0);
    SetValue(yn, 1);
    SetDirty(true);
  }

  void OnMouseDblClick(float x, float y, const IMouseMod &mod) override {}

  void OnTextEntryCompletion(const char *str, int valIdx) override {
    const int id = mEditingCorner;
    mEditingCorner = -1;
    if (id < 0)
      return;
    double v;
    if (id == kCornerBw) {
      char *end = nullptr;
      v = std::strtod(str, &end);
      if (end == str)
        return;
    } else if (!ParseFreq(str, v))
      return;
    if (mHooks.editCorner)
      mHooks.editCorner(id, v);
  }

  void DrawWidget(IGraphics &g) override {
    DrawTrack(g);
    DrawSpectrum(g);
    const IRECT tb = PlotRect();
    const float xpos = (float)GetValue(0) * tb.W();
    const float ypos = (float)GetValue(1) * tb.H();
    const IRECT hb(tb.L + xpos - mHandleRadius, tb.B - ypos - mHandleRadius, tb.L + xpos + mHandleRadius,
                   tb.B - ypos + mHandleRadius);
    if (mRandomMapOn[0] || mRandomMapOn[1]) {
      const IParam *pf = GetParam(0);
      const IParam *pb = GetParam(1);
      const double actF = std::clamp(pf->FromNormalized(GetValue(0)) * std::exp2((double)mRandomFreqOct), 20., 20000.);
      const double actBw = std::clamp(pb->FromNormalized(GetValue(1)) * std::exp2((double)mRandomBwOct * 0.5), 1., 31.);
      const float gx = tb.L + (float)pf->ToNormalized(actF) * tb.W();
      const float gy = tb.B - (float)pb->ToNormalized(actBw) * tb.H();
      if (mRandomMapOn[0] && mRandomMapOn[1]) {
        g.FillCircle(RandomColorGhost(mRandomMapColor[1]), gx, gy, mHandleRadius);
        g.FillCircle(RandomColor(mRandomMapColor[0]), gx, gy, mHandleRadius * 0.6f);
        g.FillCircle(COL_100(), gx, gy, mHandleRadius * 0.25f);
      } else {
        g.FillCircle(RandomColorGhost(mRandomMapOn[0] ? mRandomMapColor[0] : mRandomMapColor[1]), gx, gy,
                     mHandleRadius);
        g.FillCircle(COL_100(), gx, gy, mHandleRadius * 0.25f);
      }
    }
    DrawHandle(g, tb, hb);
  }

  void DrawTrack(IGraphics &g) override {
    const IRECT tb = PlotRect();
    const IParam *pf = GetParam(0);
    auto xOf = [&](double f) { return tb.L + (float)pf->ToNormalized(f) * tb.W(); };

    struct Band {
      double lo, hi;
      float v0, v1;
    };
    static const Band kBands[] = {
        {20., 100., 194.f, 218.f},
        {100., 1000., 205.f, 229.f},
        {1000., 10000., 216.f, 240.f},
        {10000., 20000., 227.f, 227.f},
    };

    for (const Band &band : kBands) {
      double edges[16];
      int n = 0;
      edges[n++] = band.lo;
      for (int decade = 1; decade <= 10000; decade *= 10)
        for (int m = 1; m <= 9; ++m) {
          const double f = m * decade;
          if (f > band.lo && f < band.hi)
            edges[n++] = f;
        }
      edges[n++] = band.hi;

      float xs[16];
      for (int i = 0; i < n; ++i)
        xs[i] = xOf(edges[i]);

      const int cells = n - 1;
      for (int i = 0; i < cells; ++i) {
        const float t = (cells > 1) ? (float)i / (cells - 1) : 0.f;
        const int v = (int)std::lround(band.v0 + (band.v1 - band.v0) * t);
        g.FillRect(WarmGray(v), IRECT(xs[i], tb.T, xs[i + 1], tb.B));
      }
    }
  }

  void DrawHandle(IGraphics &g, const IRECT &, const IRECT &handleBounds) override {
    const float cx = handleBounds.MW();
    const float cy = handleBounds.MH();
    g.FillCircle(COL_900(), cx, cy, mHandleRadius);
    g.FillCircle(COL_100(), cx, cy, mHandleRadius * 0.25f);
  }

  bool IsHit(float x, float y) const override {
    if (mTargetRECT.Contains(x, y))
      return true;
    for (int id : {kCornerCenter, kCornerBw})
      if (CornerRect(id).Contains(x, y))
        return true;
    if (mSlopeRect.Contains(x, y))
      return true;
    if (PassRejectRect().Contains(x, y))
      return true;
    if (SideLabelRect().Contains(x, y))
      return true;
    return false;
  }

  void DrawSideLabel(IGraphics &g) {
    if (mSideLabel.GetLength() == 0)
      return;
    const IRECT r = SideLabelRect();
    IText t(40, COL_500(), kFontBold, EAlign::Center, EVAlign::Middle, -90.f);
    g.DrawText(t, mSideLabel.Get(), r);
  }

  void DrawSpectrum(IGraphics &g) {
    const IRECT tb = PlotRect();
    if (tb.W() <= 0.f || tb.H() <= 0.f)
      return;
    if (mSpectrumIn.empty() || mSpectrumOut.empty() || mNumBins <= 0)
      return;

    const IParam *pf = GetParam(0);
    const double binHz = mSampleRate / std::max((double)mNumBins * 2.0, 1.0);

    auto drawFill = [&](const std::vector<float> &spec, const IColor &topColor, const IColor &bottomColor) {
      mSpecPts.clear();
      std::fill(mBandMax.begin(), mBandMax.end(), 0.f);
      std::fill(mBandUsed.begin(), mBandUsed.end(), 0);
      {
        const double logLo = std::log2(kSpecFreqLo);
        const double logHi = std::log2(kSpecFreqHi);
        const double logBand = (logHi - logLo) / kSpectrumBands;
        for (int i = 0; i < mNumBins && i < (int)spec.size(); ++i) {
          const double f = (double)i * binHz;
          if (f < kSpecFreqLo || f > kSpecFreqHi)
            continue;
          const int b = (int)((std::log2(f) - logLo) / logBand);
          if (b < 0 || b >= kSpectrumBands)
            continue;
          const float amp = spec[i];
          if (amp > mBandMax[b])
            mBandMax[b] = amp;
          mBandUsed[b] = 1;
        }
        for (int b = 0; b < kSpectrumBands; ++b) {
          if (!mBandUsed[b])
            continue;
          const double fCenter = kSpecFreqLo * std::exp2(logBand * (b + 0.5));
          const float x = tb.L + (float)pf->ToNormalized(fCenter) * tb.W();
          const float amp = mBandMax[b];
          const float db =
              (amp > 1e-6f) ? std::clamp(20.f * std::log10(amp), kSpectrumBottomDb, 0.f) : kSpectrumBottomDb;
          const float y = tb.B - (db - kSpectrumBottomDb) / (0.f - kSpectrumBottomDb) * tb.H();
          mSpecPts.push_back({x, y});
        }
      }
      std::vector<Pt> &pts = mSpecPts;
      if (pts.size() < 2)
        return;

      pts.front().x = tb.L + (float)pf->ToNormalized(kSpecFreqLo) * tb.W();
      pts.back().x = tb.L + (float)pf->ToNormalized(kSpecFreqHi) * tb.W();

      g.PathClear();
      g.PathMoveTo(pts[0].x, pts[0].y);
      if (pts.size() > 3) {
        const float s = 0.6f;
        const int n = (int)pts.size();
        for (int i = 0; i < n - 1; ++i) {
          const Pt &p0 = pts[std::max(i - 1, 0)];
          const Pt &p1 = pts[i];
          const Pt &p2 = pts[i + 1];
          const Pt &p3 = pts[std::min(i + 2, n - 1)];
          const float c1x = p1.x + (p2.x - p0.x) * (s / 6.f);
          const float c1y = p1.y + (p2.y - p0.y) * (s / 6.f);
          const float c2x = p2.x - (p3.x - p1.x) * (s / 6.f);
          const float c2y = p2.y - (p3.y - p1.y) * (s / 6.f);
          g.PathCubicBezierTo(c1x, c1y, c2x, c2y, p2.x, p2.y);
        }
      } else {
        for (int i = 1; i < (int)pts.size(); ++i)
          g.PathLineTo(pts[i].x, pts[i].y);
      }
      g.PathLineTo(pts.back().x, tb.B);
      g.PathLineTo(pts[0].x, tb.B);
      g.PathClose();

      IPattern fill = IPattern::CreateLinearGradient(tb, EDirection::Vertical,
                                                     {IColorStop(topColor, 0.f), IColorStop(bottomColor, 1.f)});
      g.PathFill(fill);
    };

    const IColor cIn = COL_500();
    const IColor cOut = COL_900();
    drawFill(mSpectrumIn, IColor(110, cIn.R, cIn.G, cIn.B), IColor(0, cIn.R, cIn.G, cIn.B));
    drawFill(mSpectrumOut, IColor(170, cOut.R, cOut.G, cOut.B), IColor(0, cOut.R, cOut.G, cOut.B));
  }

private:
  IRECT PlotRect() const {
    const IRECT &w = mWidgetBounds;
    const float top = w.T + kTopPad;
    return IRECT(w.L, top, w.R, w.B);
  }

  IRECT CornerRect(int id) const {
    const IRECT &w = mWidgetBounds;
    switch (id) {
    case kCornerCenter:
      return IRECT(w.L, w.T - kSideH, w.L + kCornerW, w.T + kCornerTextH);
    case kCornerBw:
      return IRECT(w.R - kCornerW, w.T - kSideH, w.R, w.T + kCornerTextH);
    }
    return IRECT();
  }

  IRECT SideLabelRect() const {
    const IRECT &w = mWidgetBounds;
    return IRECT(w.L + kSideLabelX, w.T + kTopPad, w.L + kSideW + kSideLabelX, w.B);
  }

  void DrawCorner(IGraphics &g, int id) {
    const IRECT r = CornerRect(id);
    const bool far = (id == kCornerBw);
    const EAlign align = far ? EAlign::Far : EAlign::Near;
    const IText t(20, COL_900(), kFontSemiBold, align, EVAlign::Middle);
    const char *prefix = far ? mBwPrefix.Get() : mCenterPrefix.Get();
    WDL_String value;
    GetCornerValue(id, value, true);
    const float cy = r.MH();
    IRECT measured;
    if (far) {
      g.MeasureText(t, value.Get(), measured);
      mRandomSwatchRect[1] = IRECT(r.R - AG_SWATCH, cy - AG_SWATCH * 0.5f, r.R, cy + AG_SWATCH * 0.5f);
      mCornerValueRect[1] =
          IRECT(mRandomSwatchRect[1].L - kSwatchGap - measured.W(), r.T, mRandomSwatchRect[1].L - kSwatchGap, r.B);
      g.DrawText(t, prefix, IRECT(r.L, r.T, mCornerValueRect[1].L - LABEL_VALUE_GAP, r.B));
      g.DrawText(t, value.Get(), mCornerValueRect[1]);
    } else {
      g.MeasureText(t, prefix, measured);
      const float prefixW = measured.W();
      g.MeasureText(t, value.Get(), measured);
      mCornerValueRect[0] =
          IRECT(r.L + prefixW + LABEL_VALUE_GAP, r.T, r.L + prefixW + LABEL_VALUE_GAP + measured.W(), r.B);
      mRandomSwatchRect[0] = IRECT(mCornerValueRect[0].R + kSwatchGap, cy - AG_SWATCH * 0.5f,
                                   mCornerValueRect[0].R + kSwatchGap + AG_SWATCH, cy + AG_SWATCH * 0.5f);
      g.DrawText(t, prefix, r);
      g.DrawText(t, value.Get(), mCornerValueRect[0]);
    }
    const int i = far ? 1 : 0;
    g.FillRect(mRandomMapOn[i] ? RandomColor(mRandomMapColor[i]) : RandomColorDim(mRandomMapColor[i]),
               mRandomSwatchRect[i].GetPadded(-1.f));
  }

  IRECT CornerValueRect(int id) const { return mCornerValueRect[id == kCornerBw]; }

  IRECT SlopeRect() const {
    const IRECT &w = mWidgetBounds;
    return IRECT(w.L + kCornerW, w.T - kSideH, w.R - kCornerW, w.T + kCornerTextH);
  }

  void DrawSlope(IGraphics &g) {
    const IRECT r = SlopeRect();
    const IText t(20, COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Middle);

    WDL_String value;
    GetSlopeValue(value);
    IRECT m1, m2;
    g.MeasureText(t, mSlopePrefix.Get(), m1);
    g.MeasureText(t, value.Get(), m2);
    const float totalW = m1.W() + LABEL_VALUE_GAP + m2.W();
    const float x0 = r.MW() - totalW * 0.5f;
    mSlopeRect = IRECT(x0, r.T, x0 + totalW, r.B);
    g.DrawText(t, mSlopePrefix.Get(), IRECT(x0, r.T, x0 + m1.W(), r.B));
    g.DrawText(t, value.Get(), IRECT(x0 + m1.W() + LABEL_VALUE_GAP, r.T, x0 + totalW, r.B));
  }

  void DrawPassToggle(IGraphics &g) {
    const IText t(20, COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Middle);
    const char *label = mPass ? orm::Tr(orm::kTxtPass, orm::UILang()) : orm::Tr(orm::kTxtReject, orm::UILang());
    IRECT m;
    g.MeasureText(t, label, m);
    const float btnW = m.W() + kPassBtnPad * 2.f;

    const float prefixRight = mCornerValueRect[1].L - LABEL_VALUE_GAP;
    g.MeasureText(t, mBwPrefix.Get(), m);
    const float prefixLeft = prefixRight - m.W();
    const IRECT b(prefixLeft - kPassGap - btnW, mWidgetBounds.T, prefixLeft - kPassGap, mWidgetBounds.T + kCornerTextH);
    mPassRejectRect = b;

    const bool hover = mPassBtnHover;
    const IColor fill = mPass ? (hover ? COL_500() : COL_300()) : COL_900();
    g.FillRect(fill, b.GetPadded(-1.f));
    const IText bt(20, mPass ? COL_900() : COL_100(), kFontSemiBold, EAlign::Center, EVAlign::Middle);
    g.DrawText(bt, label, b);
  }

  void GetSlopeValue(WDL_String &out) const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d dB/oct", kSlopeDb[std::clamp(mSlopeIndex, 0, 3)]);
    out.Set(buf);
  }

  void OpenSlopeMenu() {
    if (!GetUI())
      return;
    mSlopeMenu.Clear();
    mSlopeMenu.SetFunction([this](IPopupMenu *menu) {
      const int idx = menu ? menu->GetChosenItemIdx() : -1;
      if (idx < 0 || idx >= 4)
        return;
      if (mHooks.editSlope)
        mHooks.editSlope(kSlopeDb[idx]);
    });
    for (int i = 0; i < 4; ++i) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%d dB/oct", kSlopeDb[i]);
      mSlopeMenu.AddItem(buf);
    }
    mSlopeMenu.CheckItemAlone(std::clamp(mSlopeIndex, 0, 3));
    GetUI()->CreatePopupMenu(*this, mSlopeMenu, mSlopeRect, kNoValIdx);
  }

  void OpenRandomColorMenu(int i) {
    if (!GetUI())
      return;
    OpenColorPopup(*GetUI(), *this, mRandomMenu, mRandomSwatchRect[i], mRandomMapColor[i], [this, i](int idx) {
      if (mHooks.randomSetColor)
        mHooks.randomSetColor(i ? kCornerBw : kCornerCenter, idx);
    });
  }

  void GetCornerValue(int id, WDL_String &out, bool withUnit, bool withMod = true) const {
    char buf[32];
    if (id == kCornerBw) {
      double bw = GetParam(1)->FromNormalized(GetValue(1));
      if (withMod && mRandomMapOn[1])
        bw *= std::exp2((double)mRandomBwOct * 0.5);
      std::snprintf(buf, 32, "%.2f", bw);
    } else {
      const IParam *pf = GetParam(0);
      double c = pf->FromNormalized(GetValue(0));
      if (withMod && mRandomMapOn[0])
        c *= std::exp2((double)mRandomFreqOct);
      FormatFreq(buf, 32, c, withUnit);
    }
    out.Set(buf);
  }

  static constexpr float kCornerW = 170.f;
  static constexpr float kCornerTextH = 22.f;
  static constexpr float kPassGap = 8.f;
  static constexpr float kPassBtnPad = 6.f;
  static constexpr float kSideW = 24.f;
  static constexpr float kSideH = 0.f;
  static constexpr float kTopPad = 30.f;
  static constexpr float kSideLabelX = 4.f;
  static constexpr float kSwatchGap = 6.f;

  static constexpr float kSpectrumBottomDb = -85.f;

  static constexpr int kSpectrumBands = 256;
  static constexpr float kSpecFreqLo = 20.f;
  static constexpr float kSpecFreqHi = 20000.f;

  Hooks mHooks;
  WDL_String mSideLabel;
  bool mGhost = false;
  WDL_String mCenterPrefix{"CENTER"};
  WDL_String mBwPrefix{"WIDTH"};
  WDL_String mSlopePrefix{"SLOPE"};
  IRECT mCornerValueRect[2];
  IRECT mSlopeRect;
  IPopupMenu mSlopeMenu;
  IPopupMenu mRandomMenu;
  int mEditingCorner = -1;
  int mSlopeIndex = kSlopeDefaultIdx;
  bool mRandomMapOn[2] = {false, false};
  int mRandomMapColor[2] = {0, 0};
  bool mPass = true;
  bool mPassBtnHover = false;
  IRECT mPassRejectRect;
  IRECT mRandomSwatchRect[2];
  float mRandomFreqOct = 0.f;
  float mRandomBwOct = 0.f;

  std::vector<float> mSpectrumIn;
  std::vector<float> mSpectrumOut;
  float mAttackCoeff = 0.2f;
  float mReleaseCoeff = 0.9f;
  int mNumBins = 2048;
  double mSampleRate = 48000.0;

  struct Pt {
    float x, y;
  };
  std::vector<Pt> mSpecPts;    // 预分配: 频谱填充点
  std::vector<float> mBandMax; // 预分配: 每 band 峰值
  std::vector<char> mBandUsed; // 预分配: band 是否有数据
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
