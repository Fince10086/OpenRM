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
//
// L/R 双声道配色 (色环等边三角, 仅色相不同):
//   L = 主题色相 - 120° (超过 0/360 循环), R = 主题色相 + 120°
//   两条曲线之下、较低一条到框底的"重合区"用当前主题色相填充
//   饱和度跟随主题档位 (0/15/30/50): 重合区 = 主题档位; L/R 区域保底 15
// 渐变规则 (方案2): 锚定到填充块自身, 曲线最高点满不透明度,
//   向下衰减到 kGradientMinAlpha, 弱信号贴底时仍清晰可见。

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
    kMsgTagRelease,
    kMsgTagRange,
  };

  SpectrumPad(const IRECT &bounds) : IControl(bounds) {
    mBandAcc.assign(kSpectrumBands, BandAcc{});
    mSpecPtsL.reserve(kSpectrumBands);
    mSpecPtsR.reserve(kSpectrumBands);
    mSpecPtsO.reserve(kSpectrumBands);
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
      mReleaseCoeff = (float)std::exp(-updatePeriod / mReleaseSec);

      const float a = mAttackCoeff, r = mReleaseCoeff;
      for (int c = 0; c < 2; ++c) {
        if (mSpectrum[c].size() != (size_t)nBins)
          mSpectrum[c].assign(nBins, 0.f);
        for (int i = 0; i < nBins; ++i) {
          const float raw = d.vals[c][i], prev = mSpectrum[c][i];
          const float coef = (raw > prev) ? a : r;
          mSpectrum[c][i] = coef * prev + (1.f - coef) * raw;
        }
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
    } else if (msgTag == kMsgTagRelease) {
      float releaseSec;
      stream.Get(&releaseSec, 0);
      mReleaseSec = std::clamp(releaseSec, 0.01f, 1.f);
    } else if (msgTag == kMsgTagRange) {
      float rangeDb;
      stream.Get(&rangeDb, 0);
      mBottomDb = -std::clamp(rangeDb, 80.f, 120.f);
    }
  }

  void Draw(IGraphics &g) override {
    g.FillRect(COL_100(), mRECT);
    DrawTrack(g);
    DrawSpectrum(g);
  }

private:
  struct Pt {
    float x, y;
  };
  struct BandAcc {
    float max[2] = {0.f, 0.f}; // 每 band 每通道峰值
    char used[2] = {0, 0};     // 每 band 每通道是否有数据
  };

  // 频率(Hz) -> 归一化 x (0..1), 与 BandPass Freq 参数 (20..20000, ShapeExp) 一致
  static float FreqNorm(double hz) {
    return (float)(std::log(std::clamp(hz, 20.0, 20000.0) / 20.0) / std::log(20000.0 / 20.0));
  }

  float XOf(IGraphics &, double f) const { return mRECT.L + FreqNorm(f) * mRECT.W(); }

  // 色相循环: 超出 0..360 时折回
  static int WrapHue(int h) {
    h %= 360;
    return h < 0 ? h + 360 : h;
  }

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
    if (mSpectrum[0].empty() || mSpectrum[1].empty() || mNumBins <= 0)
      return;
    if (mRECT.W() <= 0.f || mRECT.H() <= 0.f)
      return;

    // 通道色: L/R/重合 = 色环等边三角, 色相分别是 主题-120 / 主题 / 主题+120
    // 饱和度跟随主题档位: 重合区 = 主题档位 (0 时即中性灰);
    // L/R 区域 = 主题档位但保底 15, 保证 ±120° 色相偏移至少隐约可见。
    // 亮度沿用 COL_500 档, 随主题明暗自动跟随
    const int hue = ThemeHue();
    const float b = (ThemeMode() ? kDarkB[2] : kLightB[2]) / 100.f;
    const float sO = std::max(ThemeSatMax(), 0) / 100.f;
    const float sL = std::max(ThemeSatMax(), 15) / 100.f;
    const IColor cL = HSBToIColor(WrapHue(hue - 120), sL, b);
    const IColor cR = HSBToIColor(WrapHue(hue + 120), sL, b);
    const IColor cO = HSBToIColor(hue, sO, b);

    const double binHz = mSampleRate / std::max((double)mNumBins * 2.0, 1.0);
    const double logLo = std::log2(kSpecFreqLo);
    const double logHi = std::log2(kSpecFreqHi);
    const double logBand = (logHi - logLo) / kSpectrumBands;

    auto ampToY = [&](float amp) -> float {
      const float db =
          (amp > 1e-6f) ? std::clamp(20.f * std::log10(amp), mBottomDb, 0.f) : mBottomDb;
      return mRECT.B - (db - mBottomDb) / (0.f - mBottomDb) * mRECT.H();
    };

    mSpecPtsL.clear();
    mSpecPtsR.clear();
    mSpecPtsO.clear();
    for (auto &acc : mBandAcc)
      acc = BandAcc{};

    for (int i = 0; i < mNumBins && i < (int)mSpectrum[0].size(); ++i) {
      const double f = (double)i * binHz;
      if (f < kSpecFreqLo || f > kSpecFreqHi)
        continue;
      const int b = (int)((std::log2(f) - logLo) / logBand);
      if (b < 0 || b >= kSpectrumBands)
        continue;
      for (int c = 0; c < 2; ++c) {
        const float amp = mSpectrum[c][i];
        if (amp > mBandAcc[b].max[c])
          mBandAcc[b].max[c] = amp;
        if (amp > 1e-6f)
          mBandAcc[b].used[c] = 1;
      }
    }

    for (int b = 0; b < kSpectrumBands; ++b) {
      const BandAcc &acc = mBandAcc[b];
      const double fCenter = kSpecFreqLo * std::exp2(logBand * (b + 0.5));
      const float x = mRECT.L + FreqNorm(fCenter) * mRECT.W();
      if (acc.used[0])
        mSpecPtsL.push_back({x, ampToY(acc.max[0])});
      if (acc.used[1])
        mSpecPtsR.push_back({x, ampToY(acc.max[1])});
      // 重合区: 两条曲线之下、较低一条(y 较大)到框底, 用主题色相压在最上层
      if (acc.used[0] && acc.used[1])
        mSpecPtsO.push_back({x, std::max(ampToY(acc.max[0]), ampToY(acc.max[1]))});
    }

    DrawFill(g, mSpecPtsL, cL);
    DrawFill(g, mSpecPtsR, cR);
    DrawFill(g, mSpecPtsO, cO);
  }

  void DrawFill(IGraphics &g, std::vector<Pt> &pts, const IColor &color) {
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

    // 渐变锚定到填充块自身 (方案2): 曲线最高点 = 满不透明度,
    // 向下衰减到框底的最小不透明度。弱信号曲线即使贴底也保持清晰,
    // 不再受"绝对位置越靠下越透明"影响。
    float topY = mRECT.B;
    for (const Pt &p : pts)
      topY = std::min(topY, p.y);
    const IRECT gradRect(mRECT.L, topY, mRECT.R, mRECT.B);
    IPattern fill = IPattern::CreateLinearGradient(
        gradRect, EDirection::Vertical,
        {IColorStop(color, 0.f), IColorStop(IColor(kGradientMinAlpha, color.R, color.G, color.B), 1.f)});
    g.PathFill(fill);
  }

  static constexpr int kGradientMinAlpha = 40; // 填充底部最小不透明度 (方案2 下限)
  static constexpr int kSpectrumBands = 256;
  static constexpr float kSpecFreqLo = 20.f;
  static constexpr float kSpecFreqHi = 20000.f;

  std::vector<float> mSpectrum[2]; // 平滑后的 L/R 频谱幅度 (幅度, 非 dB)
  float mAttackCoeff = 0.2f;
  float mReleaseCoeff = 0.9f;
  float mReleaseSec = 0.2f; // 释放时间常数 (s), 由插件 Release 参数下发
  float mBottomDb = -90.f;  // 频谱显示下限 (dBFS), 由插件 Range 参数下发 (-80..-120)
  int mNumBins = 2048;
  double mSampleRate = 48000.0;

  std::vector<Pt> mSpecPtsL;    // 预分配: L 填充点
  std::vector<Pt> mSpecPtsR;    // 预分配: R 填充点
  std::vector<Pt> mSpecPtsO;    // 预分配: 重合区填充点 (主题色)
  std::vector<BandAcc> mBandAcc; // 预分配: 每 band 双通道峰值
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
