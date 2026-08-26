#pragma once

// SpectrumPad — 实时立体声频谱绘制控件
//
// 视觉特性:
// 1. 20 Hz ~ 20 kHz 对数频率坐标轴。
// 2. 双声道色彩区分: 左/右声道基于主题色相做 ±120° 旋转，各自保持自身颜色绘制 (不再因重叠切换颜色)。
// 3. 合并声道 (L+R) 以主题色相绘制 (MERGE 模式独立显示)。
// 4. 动态渐变填充: 渐变锚定于信号峰值自身，弱信号在底部依然清晰。
// 5. 动力学平滑: 独立支持 Attack（上升响应）与 Release（释放衰减）时间常数平滑。

#include "IControls.h"
#include "ISender.h"
#include "UiUtils.h"
#include "../Theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
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
    kMsgTagAttack,
    kMsgTagMode,
    kMsgTagVQTBands,
    kMsgTagReset,       // 清空平滑缓冲, 显示从头加载 (γ/BPO/模式切换时由插件下发)
    kMsgTagChanMode,    // 声道显示模式 (0: L/R, 1: MERGE)
    kMsgTagMergeAlgo,   // 合并算法 (0: PWR 功率和, 1: SUM 单声道和)
    kMsgTagLevelMeter,  // 电平表数据 (LevelMeterUiData)
  };

  SpectrumPad(const IRECT &bounds) : IControl(bounds) {
    mBandAcc.assign(kSpectrumBands, BandAcc{});
    mSpecPtsL.reserve(kSpectrumBands);
    mSpecPtsR.reserve(kSpectrumBands);
    mSpecPtsM.reserve(kSpectrumBands);
    RebuildBinToBand();
  }

  void OnMsgFromDelegate(int msgTag, int dataSize, const void *pData) override {
    IByteStream stream(pData, dataSize);

    if (msgTag == ISender<>::kUpdateMessage) {
      ISenderData<3, TDataPacket> d;
      stream.Get(&d, 0);
      // FFT: 数据 = bins (nBins 个); VQT: 数据 = band 幅度 (nBands 个)
      const int nVals = (mMode == 0) ? std::max(mNumBins, 0) : (int)mVQTFreqs.size();
      if (nVals <= 0)
        return;

      const double hop = (mMode == 0) ? (double)nVals * 2.0 / 4.0 : 1024.0;
      const double updatePeriod = hop / std::max(mSampleRate, 1.0);
      mAttackCoeff = (float)std::exp(-updatePeriod / mAttackSec);
      mReleaseCoeff = (float)std::exp(-updatePeriod / mReleaseSec);

      const float a = mAttackCoeff, r = mReleaseCoeff;
      for (int c = 0; c < 3; ++c) {
        if (mSpectrum[c].size() != (size_t)nVals)
          mSpectrum[c].assign(nVals, 0.f);
        for (int i = 0; i < nVals; ++i) {
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
      RebuildBinToBand();
    } else if (msgTag == kMsgTagFFTSize) {
      int fftSize;
      stream.Get(&fftSize, 0);
      mNumBins = std::max(fftSize / 2, 1);
      RebuildBinToBand();
    } else if (msgTag == kMsgTagRelease) {
      float releaseSec;
      stream.Get(&releaseSec, 0);
      mReleaseSec = std::clamp(releaseSec, 0.01f, 1.f);
    } else if (msgTag == kMsgTagRange) {
      float rangeDb;
      stream.Get(&rangeDb, 0);
      mBottomDb = -std::clamp(rangeDb, 80.f, 120.f);
    } else if (msgTag == kMsgTagAttack) {
      float attackSec;
      stream.Get(&attackSec, 0);
      mAttackSec = std::clamp(attackSec, 0.001f, 0.1f);
    } else if (msgTag == kMsgTagMode) {
      int mode;
      stream.Get(&mode, 0);
      mMode = (mode == 1) ? 1 : 0;
      SetDirty(false);
    } else if (msgTag == kMsgTagChanMode) {
      int chanMode;
      stream.Get(&chanMode, 0);
      mChanMode = std::clamp(chanMode, 0, 1);
      SetDirty(false);
    } else if (msgTag == kMsgTagMergeAlgo) {
      int mergeAlgo;
      stream.Get(&mergeAlgo, 0);
      mMergeAlgo = std::clamp(mergeAlgo, 0, 1);
      SetDirty(false);
    } else if (msgTag == kMsgTagVQTBands) {
      const int n = dataSize / (int)sizeof(float);
      if (n != (int)mVQTFreqs.size()) {
        for (int c = 0; c < 3; ++c)
          mSpectrum[c].clear();
      }
      mVQTFreqs.resize(n);
      if (n > 0)
        std::memcpy(mVQTFreqs.data(), pData, (size_t)n * sizeof(float));
      SetDirty(false);
    } else if (msgTag == kMsgTagLevelMeter) {
      if (dataSize != (int)sizeof(LevelMeterUiData))
        return;
      LevelMeterUiData d;
      std::memcpy(&d, pData, sizeof(d));
      mPeakL = d.peakL;
      mPeakR = d.peakR;
      mTrueL = d.trueL;
      mTrueR = d.trueR;
      mRmsL = d.rmsL;
      mRmsR = d.rmsR;
      mVuL = d.vuL;
      mVuR = d.vuR;
      mHoldL = d.holdL;
      mHoldR = d.holdR;
      mHoldSec = d.holdSec;
      mMeterMode = std::clamp(d.mode, 0, 2);
      mOverL = d.overL != 0;
      mOverR = d.overR != 0;
      SetDirty(false);
    } else if (msgTag == kMsgTagReset) {
      for (int c = 0; c < 3; ++c)
        mSpectrum[c].assign(mSpectrum[c].size(), 0.f);
      SetDirty(false);
    }
  }

  void Draw(IGraphics &g) override {
    g.FillRect(COL_100(), mRECT);
    // 图形区左对齐, 右侧让出刻度文字区 + L/R 两条电平表竖条 (读数在顶部图例行)
    const IRECT plot = mRECT.GetReducedFromRight(kDbTickW + 2.f * kGainBarW);
    DrawBackground(g, plot);
    DrawDbGrid(g, plot);
    DrawSpectrum(g, plot);
    DrawLevelMeter(g, plot);
  }

private:
  struct Pt {
    float x, y;
  };
  struct BandAcc {
    float max[3] = {0.f, 0.f, 0.f}; // 每 band 每通道峰值 (0: L, 1: R, 2: Sum)
    char used[3] = {0, 0, 0};       // 每 band 每通道是否有数据
  };

  // 频率(Hz) -> 归一化 x (0..1), 与 BandPass Freq 参数 (20..20000, ShapeExp) 一致
  static float FreqNorm(double hz) {
    return (float)(std::log(std::clamp(hz, 20.0, 20000.0) / 20.0) / std::log(20000.0 / 20.0));
  }

  float XOf(const IRECT &plot, double f) const { return plot.L + FreqNorm(f) * plot.W(); }

  // 频率 × dB 二维色块网格背景:
  // 频率维度沿用原 DrawTrack 的分段渐变 (低频偏暗, 高频偏亮);
  // dB 维度每 20 dB 一横条 (底部深, 顶部浅);
  // 每个 cell 亮度取两维度平均后过 WarmGray, 饱和度由 SatForB 自动决定。
  void DrawBackground(IGraphics &g, const IRECT &plot) {
    if (mBottomDb >= 0.f)
      return;

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

    // 收集频率 cell 的 x 边界与 v_freq (与原 DrawTrack 一致)
    struct FreqCell {
      float xL, xR, vFreq;
    };
    std::vector<FreqCell> freqCells;
    freqCells.reserve(32);
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
        xs[i] = XOf(plot, edges[i]);

      const int cells = n - 1;
      for (int i = 0; i < cells; ++i) {
        const float t = (cells > 1) ? (float)i / (cells - 1) : 0.f;
        freqCells.push_back({xs[i], xs[i + 1], band.v0 + (band.v1 - band.v0) * t});
      }
    }

    // dB 横条边界: 顶部 kTopDb, 刻度 0/-20/-40..., 底部 mBottomDb
    std::vector<float> dbBounds;
    dbBounds.push_back(kTopDb);
    for (int db = 0; db > (int)mBottomDb; db -= 20)
      dbBounds.push_back((float)db);
    dbBounds.push_back(mBottomDb);

    auto dbToY = [&](float db) -> float {
      return plot.B - (db - mBottomDb) / (kTopDb - mBottomDb) * plot.H();
    };

    // dB 维度亮度: 顶部浅, 底部深 (与频率维度同量级, 取平均后过 WarmGray)
    constexpr float kVDbTop = 245.f;
    constexpr float kVDbBottom = 172.f;
    auto vDbAt = [&](float db) -> float {
      const float t = (kTopDb - db) / (kTopDb - mBottomDb); // 0=顶, 1=底
      return kVDbTop + (kVDbBottom - kVDbTop) * t;
    };

    // 绘制二维网格 (相邻 cell 各向右侧/下侧重叠 1px, 消除抗锯齿亚像素间隙)
    const size_t nRows = dbBounds.size();
    const size_t nCols = freqCells.size();
    for (size_t r = 0; r + 1 < nRows; ++r) {
      const float yHigh = dbToY(dbBounds[r]);
      const float yLow = (r + 2 < nRows) ? dbToY(dbBounds[r + 1]) + 1.f : plot.B;
      const float vDb = vDbAt((dbBounds[r] + dbBounds[r + 1]) * 0.5f);
      for (size_t i = 0; i < nCols; ++i) {
        const auto &fc = freqCells[i];
        const float xR = (i + 1 < nCols) ? fc.xR + 1.f : plot.R;
        const int v = (int)std::lround((fc.vFreq + vDb) * 0.5f);
        g.FillRect(WarmGray(v), IRECT(fc.xL, yHigh, xR, yLow));
      }
    }
  }

  // 电平表满刻度 (显示顶部): dBTP +6 dB / dBFS 0 dBFS / VU +3 VU (-15 dBFS)。
  // 刻度网格仍固定到 kTopDb, 顶部空出的区域放置 over LED。
  float MeterTopDb() const {
    switch (mMeterMode) {
    case 1: return 0.f;
    case 2: return -15.f; // +3 VU = -15 dBFS
    default: return kTopDb;
    }
  }

  float YOf(const IRECT &plot, float db) const {
    return plot.B - (db - mBottomDb) / (kTopDb - mBottomDb) * plot.H();
  }

  // L/R 双电平表条 + over 指示 (读数在顶部图例行, 由 ChannelLegendControl 绘制)
  void DrawLevelMeter(IGraphics &g, const IRECT &plot) {
    IColor cL, cR, cM;
    GetChannelColors(cL, cR, cM);

    const float barL0 = plot.R + kDbTickW;
    const IRECT barL(barL0, plot.T, barL0 + kGainBarW, plot.B);
    const IRECT barR(barL.R, plot.T, barL.R + kGainBarW, plot.B);

    DrawMeterBar(g, plot, barL, cL, 0);
    DrawMeterBar(g, plot, barR, cR, 1);
  }

  void DrawMeterBar(IGraphics &g, const IRECT &plot, const IRECT &bar, const IColor &chan, int ch) {
    const float top = MeterTopDb();
    float val, hold = -1000.f;
    float rmsVal = -999.f;
    bool over = false;
    if (mMeterMode == 0) {
      val = ch ? mTrueR : mTrueL;
      hold = ch ? mHoldR : mHoldL;
      over = ch ? mOverR : mOverL;
    } else if (mMeterMode == 1) {
      val = ch ? mPeakR : mPeakL;
      rmsVal = ch ? mRmsR : mRmsL;
      hold = ch ? mHoldR : mHoldL;
      over = ch ? mOverR : mOverL;
    } else {
      val = ch ? mVuR : mVuL;
      hold = ch ? mHoldR : mHoldL;
      over = ch ? mOverR : mOverL;
    }
    val = std::clamp(val, mBottomDb, top);

    g.FillRect(COL_300(), bar); // 轨道底色 (与滑块条一致)

    // 分段填充: 绿区通道色 / 黄区固定黄 / 红区固定红, 统一 alpha (深浅由层决定)
    auto fillSeg = [&](float yTop, const IColor &base, int alpha) {
      const float yM18 = YOf(plot, -18.f);
      const float yM6 = YOf(plot, -6.f);
      const IColor cRed(alpha, MeterRed().R, MeterRed().G, MeterRed().B);
      const IColor cYellow(alpha, MeterYellow().R, MeterYellow().G, MeterYellow().B);
      const IColor cGreen(alpha, base.R, base.G, base.B);
      if (yTop < yM6)
        g.FillRect(cRed, IRECT(bar.L, yTop, bar.R, yM6));
      if (yTop < yM18)
        g.FillRect(cYellow, IRECT(bar.L, std::max(yTop, yM6), bar.R, yM18));
      if (yTop < bar.B)
        g.FillRect(cGreen, IRECT(bar.L, std::max(yTop, yM18), bar.R, bar.B));
    };

    if (mMeterMode == 1) {
      // dBFS+RMS: 下层 dBFS 全宽浅色, 上层 RMS 全宽深色, 粗细一致
      const float yPeak = YOf(plot, val);
      const float yRms = YOf(plot, std::clamp(rmsVal, mBottomDb, kTopDb));
      fillSeg(yPeak, chan, 90);
      fillSeg(yRms, chan, 235);
    } else {
      fillSeg(YOf(plot, val), chan, 255); // dBTP / VU 单层实色
    }

    // 峰值保持亮线
    if (mHoldSec > 0.f && hold > mBottomDb) {
      const float yH = YOf(plot, std::clamp(hold, mBottomDb, top));
      g.FillRect(COL_900(), IRECT(bar.L, yH - 1.f, bar.R, yH + 1.f));
    }

    // over 指示 (仅 dBFS): 0 dB 以上完全留空 (盖掉背景/灰色), over 时方形块顶对齐频谱表顶部
    if (mMeterMode == 1) {
      const float y0 = YOf(plot, 0.f);
      g.FillRect(COL_100(), IRECT(bar.L, plot.T, bar.R, y0));
      if (over) {
        constexpr float kGap = 3.f;
        if (y0 - kGap > plot.T + 1.f)
          g.FillRect(MeterOverLed(), IRECT(bar.L, plot.T, bar.R, y0 - kGap));
      }
    }
  }

  // 每 20dB 一档的右侧刻度文字 (网格线已由 DrawBackground 色块替代)
  void DrawDbGrid(IGraphics &g, const IRECT &plot) {
    if (mBottomDb >= 0.f)
      return;

    const int bottomDb = (int)mBottomDb;

    for (int db = 0; db >= bottomDb; db -= 20) {
      const float y = plot.B - (float)(db - bottomDb) / (kTopDb - (float)bottomDb) * plot.H();

      // 刻度文字: 位于 plot 右侧刻度区, 右对齐, 字号/字重/颜色与滑块参数值一致。
      // 顶部 0dB / 底部最后一条刻度避开控件边界, 防止文字被裁剪。
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%d", db);
      const float labelH = 28.f;
      IRECT labelR(plot.R, y - labelH * 0.5f, plot.R + kDbTickW - 4.f, y + labelH * 0.5f);
      EVAlign valign = EVAlign::Middle;
      if (labelR.T < plot.T) {
        labelR.T = plot.T + 2.f; // 贴顶: 文字整体移到刻度线下方
        labelR.B = labelR.T + labelH;
        valign = EVAlign::Top;
      } else if (labelR.B > plot.B) {
        labelR.B = plot.B - 2.f; // 贴底: 文字整体移到刻度线上方
        labelR.T = labelR.B - labelH;
        valign = EVAlign::Bottom;
      }
      g.DrawText(IText(20, COL_700(), kFontRegular, EAlign::Far, valign), buf, labelR);
    }

    // VU 模式: 0 VU (-18 dBFS) 参考刻度线
    if (mMeterMode == 2 && mBottomDb <= -18.f) {
      const float y18 = YOf(plot, -18.f);
      g.FillRect(COL_700(), IRECT(plot.R + 2.f, y18 - 1.f, plot.R + kDbTickW - 4.f, y18 + 1.f));
    }
  }

  void DrawSpectrum(IGraphics &g, const IRECT &plot) {
    if (mSpectrum[0].empty() || mSpectrum[1].empty() || mNumBins <= 0)
      return;
    if (plot.W() <= 0.f || plot.H() <= 0.f)
      return;

    // 通道色: L/R 基于主题色相 ±120°, 各自保持自身颜色 (不因重叠切换)。
    // 取色逻辑见 Theme.h GetChannelColors, 与色块图例保持一致。
    IColor cL, cR, cO;
    GetChannelColors(cL, cR, cO);

    const double logLo = std::log2(kSpecFreqLo);
    const double logHi = std::log2(kSpecFreqHi);
    const double logBand = (logHi - logLo) / kSpectrumBands;

    auto ampToY = [&](float amp) -> float {
      const float db =
          (amp > 1e-6f) ? std::clamp(20.f * std::log10(amp), mBottomDb, kTopDb) : mBottomDb;
      return plot.B - (db - mBottomDb) / (kTopDb - mBottomDb) * plot.H();
    };

    // VQT 模式: 数据 = band 幅度, 按 band 中心频率的原始对数位置直接绘制 (不做 256 band 聚合)
    if (mMode == 1) {
      mSpecPtsL.clear();
      mSpecPtsR.clear();
      mSpecPtsM.clear();
      const int nb = (int)mVQTFreqs.size();
      const int have = std::min(nb, (int)mSpectrum[0].size());
      for (int b = 0; b < have; ++b) {
        const float x = plot.L + FreqNorm(mVQTFreqs[b]) * plot.W();
        const float aL = mSpectrum[0][b];
        const float aR = mSpectrum[1][b];
        const float aSum = (mSpectrum[2].size() > (size_t)b) ? mSpectrum[2][b] : 0.f;
        const float yL = ampToY(aL);
        const float yR = ampToY(aR);
        if (aL > 1e-6f)
          mSpecPtsL.push_back({x, yL});
        if (aR > 1e-6f)
          mSpecPtsR.push_back({x, yR});

        const float aM = (mMergeAlgo == 0) ? std::sqrt(aL * aL + aR * aR) : aSum;
        if (aM > 1e-6f)
          mSpecPtsM.push_back({x, ampToY(aM)});
      }

      if (mChanMode == 0) {
        DrawFill(g, plot, mSpecPtsL, cL, kGradientMinAlpha, kLayerTopAlpha);
        DrawFill(g, plot, mSpecPtsR, cR, kGradientMinAlpha, kLayerTopAlpha);
      } else {
        DrawFill(g, plot, mSpecPtsM, cO);
      }
      return;
    }

    mSpecPtsL.clear();
    mSpecPtsR.clear();
    mSpecPtsM.clear();
    for (auto &acc : mBandAcc)
      acc = BandAcc{};

    // bin -> band 映射查表 (预计算, 见 RebuildBinToBand), 避免每帧 2048*2 次 log2
    if ((int)mBinToBand.size() != mNumBins)
      RebuildBinToBand();
    const int nb = std::min(mNumBins, (int)mSpectrum[0].size());
    for (int i = 0; i < nb; ++i) {
      const int b = mBinToBand[i];
      if (b < 0)
        continue;
      for (int c = 0; c < 3; ++c) {
        if (i < (int)mSpectrum[c].size()) {
          const float amp = mSpectrum[c][i];
          if (amp > mBandAcc[b].max[c])
            mBandAcc[b].max[c] = amp;
          if (amp > 1e-6f)
            mBandAcc[b].used[c] = 1;
        }
      }
    }

    for (int b = 0; b < kSpectrumBands; ++b) {
      const BandAcc &acc = mBandAcc[b];
      const double fCenter = kSpecFreqLo * std::exp2(logBand * (b + 0.5));
      const float x = plot.L + FreqNorm(fCenter) * plot.W();
      const float aL = acc.max[0], aR = acc.max[1], aSum = acc.max[2];
      const float yL = ampToY(aL);
      const float yR = ampToY(aR);
      if (acc.used[0])
        mSpecPtsL.push_back({x, yL});
      if (acc.used[1])
        mSpecPtsR.push_back({x, yR});

      const float aM = (mMergeAlgo == 0) ? std::sqrt(aL * aL + aR * aR) : aSum;
      if (aM > 1e-6f)
        mSpecPtsM.push_back({x, ampToY(aM)});
    }

    if (mChanMode == 0) {
      DrawFill(g, plot, mSpecPtsL, cL, kGradientMinAlpha, kLayerTopAlpha);
      DrawFill(g, plot, mSpecPtsR, cR, kGradientMinAlpha, kLayerTopAlpha);
    } else {
      DrawFill(g, plot, mSpecPtsM, cO);
    }
  }

  void DrawFill(IGraphics &g, const IRECT &plot, std::vector<Pt> &pts, const IColor &color,
                int minAlpha = kGradientMinAlpha, int topAlpha = 255) {
    if (pts.size() < 2)
      return;

    // 左右边缘闭合：低频延伸至左边缘贴底，高频在实际最高频点处垂直收口
    pts.front().x = plot.L;
    if (pts.back().x >= plot.R - plot.W() * 0.02f)
      pts.back().x = plot.R;

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
    g.PathLineTo(pts.back().x, plot.B);
    g.PathLineTo(pts[0].x, plot.B);
    g.PathClose();

    // 渐变范围跟随曲线峰值，保证弱信号在底部也有足够的对比度
    float topY = plot.B;
    for (const Pt &p : pts)
      topY = std::min(topY, p.y);
    const IRECT gradRect(plot.L, topY, plot.R, plot.B);
    // 指数衰减渐变: 顶部接近实色, 按指数曲线快速向底部透明 (多 stops 近似)
    constexpr int kGradientStops = 12;
    constexpr float kGradientDecay = 3.5f;
    IPattern fill = IPattern::CreateLinearGradient(gradRect, EDirection::Vertical);
    for (int i = 0; i < kGradientStops; ++i) {
      const float t = (float)i / (float)(kGradientStops - 1);
      const float alphaF =
          (float)minAlpha + (float)(topAlpha - minAlpha) * std::exp(-kGradientDecay * t);
      fill.AddStop(IColor((int)std::lround(std::clamp(alphaF, 0.f, 255.f)), color.R, color.G,
                          color.B),
                   t);
    }
    g.PathFill(fill);
  }

  static constexpr int kGradientMinAlpha = 10; // L/R 实体填充底部最小不透明度
  static constexpr int kLayerTopAlpha = 160;   // L/R 顶部不透明度 (从 255 降低, 更透明)
  static constexpr int kSpectrumBands = 256;
  static constexpr float kSpecFreqLo = 20.f;
  static constexpr float kSpecFreqHi = 20000.f;
  static constexpr float kTopDb = 6.f;         // 显示范围顶部 (dBFS), 刻度线仍从 0 dB 开始

  // 预计算 bin -> 对数 band 映射表: 只在采样率/FFT 尺寸变化时重建,
  // 绘制聚合循环直接查表, 省去每帧 2048*2 次 log2。
  void RebuildBinToBand() {
    const int nb = mNumBins;
    mBinToBand.assign(nb, -1);
    if (nb <= 0)
      return;
    const double binHz = mSampleRate / std::max((double)nb * 2.0, 1.0);
    const double logLo = std::log2(kSpecFreqLo);
    const double logHi = std::log2(kSpecFreqHi);
    const double logBand = (logHi - logLo) / kSpectrumBands;
    for (int i = 0; i < nb; ++i) {
      const double f = (double)i * binHz;
      if (f < kSpecFreqLo || f > kSpecFreqHi)
        continue;
      const int b = (int)((std::log2(f) - logLo) / logBand);
      if (b >= 0 && b < kSpectrumBands)
        mBinToBand[i] = b;
    }
  }

  std::vector<float> mSpectrum[3]; // 平滑后的 L/R/Sum 频谱幅度 (幅度, 非 dB)
  float mPeakL = -120.f, mPeakR = -120.f;   // 电平表: 样本峰值 dBFS (已平滑)
  float mTrueL = -120.f, mTrueR = -120.f;   // 电平表: dBTP 真峰值 (已平滑)
  float mRmsL = -120.f, mRmsR = -120.f;     // 电平表: RMS dBFS (300ms 积分)
  float mVuL = -120.f, mVuR = -120.f;       // 电平表: VU 对应 dBFS (0 VU = -18 dBFS)
  float mHoldL = -1000.f, mHoldR = -1000.f; // 电平表: 峰值保持 (显示域 dB, -1000 = 无效)
  float mHoldSec = 2.f;                     // 电平表: 保持时长 (s)
  int mMeterMode = 0;                       // 电平表模式: 0=dBTP, 1=dBFS+RMS, 2=VU
  bool mOverL = false, mOverR = false;      // 电平表: 过载锁存
  std::vector<int> mBinToBand;     // 预计算: bin -> band 映射 (-1 = 频段外)
  int mMode = 0;                   // 分析模式: 0=FFT, 1=VQT
  int mChanMode = 0;               // 声道显示模式: 0=L/R, 1=MERGE
  int mMergeAlgo = 0;              // 合并算法: 0=PWR 功率和, 1=SUM 单声道和
  std::vector<float> mVQTFreqs; // VQT band 中心频率 (Hz), 由插件下发
  float mAttackCoeff = 0.2f;
  float mReleaseCoeff = 0.9f;
  float mAttackSec = 0.05f; // 上升时间常数 (s), 由插件 Attack 参数下发
  float mReleaseSec = 0.2f; // 释放时间常数 (s), 由插件 Release 参数下发
  float mBottomDb = -90.f;  // 频谱显示下限 (dBFS), 由插件 Range 参数下发 (-80..-120)
  int mNumBins = 2048;
  double mSampleRate = 48000.0;

  std::vector<Pt> mSpecPtsL;    // 预分配: L 填充点
  std::vector<Pt> mSpecPtsR;    // 预分配: R 填充点
  std::vector<Pt> mSpecPtsM;    // 预分配: 合并声道 (L+R) 填充点
  std::vector<BandAcc> mBandAcc; // 预分配: 每 band 双通道峰值
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
