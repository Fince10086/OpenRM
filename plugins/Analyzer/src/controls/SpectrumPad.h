#pragma once

// SpectrumPad — Analyzer 的"空 xypad": 只显示频谱, 不做任何参数编辑。
//
// 从 BandPass 的 FilterNodePad 裁剪而来 (共用其设计语言):
//   保留: 对数频率轨道背景 (WarmGray 分段)、频谱平滑 (attack/release)、
//         同一套消息协议 (ISender kUpdateMessage / sampleRate / fftSize)
//   删除: 控制点/手柄、中心/带宽读数、滚降菜单、通过/抑制切换、
//         随机映射色块、L/R 侧标、Ghost 层 —— 即"空 pad"。
// 频率->x 映射与 BandPass 的 ShapeExp 参数 (20..20000) 归一化一致:
//   x = L + W * ln(f/20)/ln(1000), 保证频段边界位置与 BandPass 完全对齐。

#include "IControls.h"
#include "ISender.h"
#include "UiUtils.h"
#include "../Theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class SpectrumPad : public IControl {
public:
  using TDataPacket = std::array<float, 4096>;

  enum MsgTags {
    kMsgTagSampleRate = 1,
    kMsgTagFFTSize,
  };

  SpectrumPad(const IRECT &bounds) : IControl(bounds) {
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

      if (mSpectrum.size() != (size_t)nBins)
        mSpectrum.assign(nBins, 0.f);
      const float a = mAttackCoeff, r = mReleaseCoeff;
      for (int i = 0; i < nBins; ++i) {
        const float raw = d.vals[0][i], prev = mSpectrum[i];
        const float coef = (raw > prev) ? a : r;
        mSpectrum[i] = coef * prev + (1.f - coef) * raw;
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

  void Draw(IGraphics &g) override {
    g.FillRect(COL_100(), mRECT);
    DrawTrack(g);
    DrawSpectrum(g);
  }

private:
  // 频率(Hz) -> 归一化 x (0..1), 与 BandPass Freq 参数 (20..20000, ShapeExp) 一致
  static float FreqNorm(double hz) {
    return (float)(std::log(std::clamp(hz, 20.0, 20000.0) / 20.0) / std::log(20000.0 / 20.0));
  }

  float XOf(IGraphics &, double f) const { return mRECT.L + FreqNorm(f) * mRECT.W(); }

  void DrawTrack(IGraphics &g) {
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
        xs[i] = XOf(g, edges[i]);

      const int cells = n - 1;
      for (int i = 0; i < cells; ++i) {
        const float t = (cells > 1) ? (float)i / (cells - 1) : 0.f;
        const int v = (int)std::lround(band.v0 + (band.v1 - band.v0) * t);
        g.FillRect(WarmGray(v), IRECT(xs[i], mRECT.T, xs[i + 1], mRECT.B));
      }
    }
  }

  void DrawSpectrum(IGraphics &g) {
    if (mSpectrum.empty() || mNumBins <= 0)
      return;
    if (mRECT.W() <= 0.f || mRECT.H() <= 0.f)
      return;

    // 输入频谱填充色: 与 BandPass 中"输入"曲线的配色一致 (COL_500 渐变)。
    // 每帧取色, 保证设置面板改主题/色相后立即刷新。
    const IColor cIn = COL_500();

    const double binHz = mSampleRate / std::max((double)mNumBins * 2.0, 1.0);

    mSpecPts.clear();
    std::fill(mBandMax.begin(), mBandMax.end(), 0.f);
    std::fill(mBandUsed.begin(), mBandUsed.end(), 0);
    {
      const double logLo = std::log2(kSpecFreqLo);
      const double logHi = std::log2(kSpecFreqHi);
      const double logBand = (logHi - logLo) / kSpectrumBands;
      for (int i = 0; i < mNumBins && i < (int)mSpectrum.size(); ++i) {
        const double f = (double)i * binHz;
        if (f < kSpecFreqLo || f > kSpecFreqHi)
          continue;
        const int b = (int)((std::log2(f) - logLo) / logBand);
        if (b < 0 || b >= kSpectrumBands)
          continue;
        const float amp = mSpectrum[i];
        if (amp > mBandMax[b])
          mBandMax[b] = amp;
        mBandUsed[b] = 1;
      }
      for (int b = 0; b < kSpectrumBands; ++b) {
        if (!mBandUsed[b])
          continue;
        const double fCenter = kSpecFreqLo * std::exp2(logBand * (b + 0.5));
        const float x = mRECT.L + FreqNorm(fCenter) * mRECT.W();
        const float amp = mBandMax[b];
        const float db =
            (amp > 1e-6f) ? std::clamp(20.f * std::log10(amp), kSpectrumBottomDb, 0.f) : kSpectrumBottomDb;
        const float y = mRECT.B - (db - kSpectrumBottomDb) / (0.f - kSpectrumBottomDb) * mRECT.H();
        mSpecPts.push_back({x, y});
      }
    }
    std::vector<Pt> &pts = mSpecPts;
    if (pts.size() < 2)
      return;

    pts.front().x = mRECT.L + FreqNorm(kSpecFreqLo) * mRECT.W();
    pts.back().x = mRECT.L + FreqNorm(kSpecFreqHi) * mRECT.W();

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
    g.PathLineTo(pts.back().x, mRECT.B);
    g.PathLineTo(pts[0].x, mRECT.B);
    g.PathClose();

    IPattern fill = IPattern::CreateLinearGradient(mRECT, EDirection::Vertical,
                                                   {IColorStop(cIn, 0.f),
                                                    IColorStop(IColor(0, cIn.R, cIn.G, cIn.B), 1.f)});
    g.PathFill(fill);
  }

  static constexpr float kSpectrumBottomDb = -85.f;
  static constexpr int kSpectrumBands = 256;
  static constexpr float kSpecFreqLo = 20.f;
  static constexpr float kSpecFreqHi = 20000.f;

  struct Pt {
    float x, y;
  };
  std::vector<float> mSpectrum; // 平滑后的输入频谱幅度 (幅度, 非 dB)
  float mAttackCoeff = 0.2f;
  float mReleaseCoeff = 0.9f;
  int mNumBins = 2048;
  double mSampleRate = 48000.0;

  std::vector<Pt> mSpecPts;    // 预分配: 频谱填充点
  std::vector<float> mBandMax; // 预分配: 每 band 峰值
  std::vector<char> mBandUsed; // 预分配: band 是否有数据
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
