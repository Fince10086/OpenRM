#pragma once

// SpectrumPad — 实时频谱与电平显示控件

#include "IControls.h"
#include "ISender.h"
#include "UiUtils.h"
#include "../Theme.h"
#include "../dsp/FastMath.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class SpectrumPad : public IControl {
public:
  // 尺寸必须与四个分析引擎的数据包严格一致 (SpectrumSTFT/VQT/PBT/MRFFT 均为 8192):
  // ISender 数据包整体拷贝, 这里按同尺寸结构体读取, 不一致会整包读取失败。
  using TDataPacket = std::array<float, 8192>;

  enum MsgTags {
    kMsgTagSampleRate = 1,
    kMsgTagFFTSize,
    kMsgTagRelease,
    kMsgTagRange,
    kMsgTagAttack,
    kMsgTagSlope,       // 频谱斜率 (dB/oct, 当前模式生效值; FFT 与逐 band 引擎档值不同)
    kMsgTagMode,
    kMsgTagVQTBands,
    kMsgTagReset,       // 清空平滑缓冲, 显示从头加载 (γ/BPO/模式切换时由插件下发)
    kMsgTagChanMode,    // 声道显示模式 (0: L/R, 1: MERGE)
    kMsgTagMergeAlgo,   // 合并算法 (0: PWR 功率和, 1: SUM 单声道和)
    kMsgTagLevelMeter,  // 电平表数据 (LevelMeterUiData)
    kMsgTagPBTBands,    // PBT 频带中心频率 (Hz)
    kMsgTagMRFFTBands,  // MR-FFT 频带中心频率 (Hz)
    kMsgTagRTABands,    // RTA 频带中心频率 (Hz)
  };

  SpectrumPad(const IRECT &bounds) : IControl(bounds) {
    mBandAcc.assign(kSpectrumBands, BandAcc{});
    mSpecPtsL.reserve(kSpectrumBands);
    mSpecPtsR.reserve(kSpectrumBands);
    mSpecPtsM.reserve(kSpectrumBands);
    RebuildBinToBand();
    // 预计算 256 个 band 的频率归一化位置 (对数频率轴恒定点), 绘制时只做一次乘加
    const double logLo = std::log2(kSpecFreqLo);
    const double logHi = std::log2(kSpecFreqHi);
    const double logBand = (logHi - logLo) / kSpectrumBands;
    for (int b = 0; b < kSpectrumBands; ++b) {
      const double fCenter = kSpecFreqLo * std::exp2(logBand * (b + 0.5));
      mBandNormX[b] = FreqNorm(fCenter);
    }
    mHoldPts.reserve(kSpectrumBands);
  }

  // 清空频谱峰值保持 (RESET 按钮联动; hold 关闭时由 UpdatePeakHold 自动调用一次)
  void ClearPeakHold() {
    for (int c = 0; c < 3; ++c) {
      mHoldSpec[c].assign(mHoldSpec[c].size(), 0.f);
      mHoldAge[c].assign(mHoldAge[c].size(), 0.f);
    }
    mHoldSignal = false;
    SetDirty(false);
  }

  void OnMsgFromDelegate(int msgTag, int dataSize, const void *pData) override {
    IByteStream stream(pData, dataSize);

    if (msgTag == ISender<>::kUpdateMessage) {
      ISenderData<3, TDataPacket> d;
      stream.Get(&d, 0);
      // FFT: 数据 = bins (nBins 个); VQT/PBT/MR-FFT/RTA: 数据 = band 幅度 (nBands 个)
      const int nVals = (mMode == 0) ? std::max(mNumBins, 0)
                                     : (mMode == 1) ? (int)mVQTFreqs.size()
                                     : (mMode == 2) ? (int)mPBTFreqs.size()
                                     : (mMode == 3) ? (int)mMRFFTFreqs.size()
                                                    : (int)mRTAFreqs.size();
      if (nVals <= 0)
        return;

      // 帧进给周期恒 1024 样本: FFT 档位 overlap 规则 (2048/2, 4096/4, 8192/8)
      // 保证 hop 恒 1024, 其余引擎 kHop 固定 1024 —— 平滑时间常数与引擎同步
      const double hop = 1024.0;
      const double updatePeriod = hop / std::max(mSampleRate, 1.0);
      mAttackCoeff = (float)std::exp(-updatePeriod / mAttackSec);
      mReleaseCoeff = (float)std::exp(-updatePeriod / mReleaseSec);

      const float a = mAttackCoeff, r = mReleaseCoeff;
      if (mMode == 2 && mPBTFreqs.size() == (size_t)nVals) {
        for (int c = 0; c < 3; ++c) {
          if (mSpectrum[c].size() != (size_t)nVals)
            mSpectrum[c].assign(nVals, 0.f);
          for (int i = 0; i < nVals; ++i) {
            const float raw = d.vals[c][i], prev = mSpectrum[c][i];
            const float fc = mPBTFreqs[i];
            const float bw = (fc < 250.f)
                ? ((i > 0 && mPBTFreqs[i] < 250.f) ? (mPBTFreqs[i] - mPBTFreqs[i - 1]) : 40.f)
                : (fc * 0.1f);
            // 物理起振时间常数 τ = 1 / (π · bw): 窄带低频展现自然蓄力爬坡感
            const float tauBand = std::max(mAttackSec, 1.f / (3.14159f * std::max(bw, 5.f)));
            const float aBand = (float)std::exp(-updatePeriod / tauBand);
            const float coef = (raw > prev) ? aBand : r;
            mSpectrum[c][i] = coef * prev + (1.f - coef) * raw;
          }
        }
      } else {
        for (int c = 0; c < 3; ++c) {
          if (mSpectrum[c].size() != (size_t)nVals)
            mSpectrum[c].assign(nVals, 0.f);
          for (int i = 0; i < nVals; ++i) {
            const float raw = d.vals[c][i], prev = mSpectrum[c][i];
            const float coef = (raw > prev) ? a : r;
            mSpectrum[c][i] = coef * prev + (1.f - coef) * raw;
          }
        }
      }
      UpdatePeakHold(nVals);
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
    } else if (msgTag == kMsgTagSlope) {
      float slopeDb;
      stream.Get(&slopeDb, 0);
      mSlopeDbPerOct = slopeDb;
      RebuildSlopeGain();
      SetDirty(false);
    } else if (msgTag == kMsgTagMode) {
      int mode;
      stream.Get(&mode, 0);
      mMode = std::clamp(mode, 0, 4);
      RebuildSlopeGain(); // 斜率档值随模式变化 (FFT 0/3/4.5, 其余 -3/0/1.5)
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
      mVQTFreqNorm.resize(n);
      for (int i = 0; i < n; ++i)
        mVQTFreqNorm[i] = FreqNorm(mVQTFreqs[i]);
      RebuildSlopeGain(); // 斜率增益依赖 band 中心频率, band 表更新后重建
      SetDirty(false);
    } else if (msgTag == kMsgTagPBTBands) {
      const int n = dataSize / (int)sizeof(float);
      if (n != (int)mPBTFreqs.size()) {
        for (int c = 0; c < 3; ++c)
          mSpectrum[c].clear();
      }
      mPBTFreqs.resize(n);
      if (n > 0)
        std::memcpy(mPBTFreqs.data(), pData, (size_t)n * sizeof(float));
      mPBTFreqNorm.resize(n);
      for (int i = 0; i < n; ++i)
        mPBTFreqNorm[i] = FreqNorm(mPBTFreqs[i]);
      RebuildSlopeGain(); // 斜率增益依赖 band 中心频率, band 表更新后重建
      SetDirty(false);
    } else if (msgTag == kMsgTagMRFFTBands) {
      const int n = dataSize / (int)sizeof(float);
      if (n != (int)mMRFFTFreqs.size()) {
        for (int c = 0; c < 3; ++c)
          mSpectrum[c].clear();
      }
      mMRFFTFreqs.resize(n);
      if (n > 0)
        std::memcpy(mMRFFTFreqs.data(), pData, (size_t)n * sizeof(float));
      mMRFFTFreqNorm.resize(n);
      for (int i = 0; i < n; ++i)
        mMRFFTFreqNorm[i] = FreqNorm(mMRFFTFreqs[i]);
      RebuildSlopeGain(); // 斜率增益依赖 band 中心频率, band 表更新后重建
      SetDirty(false);
    } else if (msgTag == kMsgTagRTABands) {
      const int n = dataSize / (int)sizeof(float);
      if (n != (int)mRTAFreqs.size()) {
        for (int c = 0; c < 3; ++c)
          mSpectrum[c].clear();
      }
      mRTAFreqs.resize(n);
      if (n > 0)
        std::memcpy(mRTAFreqs.data(), pData, (size_t)n * sizeof(float));
      mRTAFreqNorm.resize(n);
      for (int i = 0; i < n; ++i)
        mRTAFreqNorm[i] = FreqNorm(mRTAFreqs[i]);
      RebuildSlopeGain(); // 斜率增益依赖 band 中心频率, band 表更新后重建
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
      ClearPeakHold();
      SetDirty(false);
    }
  }

  // hover 十字准线: IGraphics 对悬停控件每次鼠标移动都会回调 OnMouseOver (带新坐标),
  // 这里实时记录位置并请求重绘; 离开控件时清除。仅显示用途, 不捕获鼠标。
  void OnMouseOver(float x, float y, const IMouseMod &mod) override {
    mMouseIsOver = true;
    if (!mHoverActive || mHoverX != x || mHoverY != y) {
      mHoverX = x;
      mHoverY = y;
      mHoverActive = true;
      SetDirty(false);
    }
  }

  void OnMouseOut() override {
    mMouseIsOver = false;
    if (mHoverActive) {
      mHoverActive = false;
      SetDirty(false);
    }
  }

  void Draw(IGraphics &g) override {
    g.FillRect(COL_100(), mRECT);
    // 图形区左对齐, 右侧让出 L/R 两条电平表竖条 (刻度文字绘制在频谱区域内部右侧)。
    // 对 plot 做物理像素对齐: 层位图/层内绘制/贴图/频谱/电平表共用同一矩形,
    // 避免层内容与位图边缘之间的亚像素透明条带在右侧/底侧露出底色 (白边)。
    const IRECT plot =
        mRECT.GetReducedFromRight(2.f * kGainBarW).GetPixelAligned(g.GetScreenScale() * g.GetDrawScale());

    // hover 十字准线: 竖线 (1px) 在光标 x, 标签位于竖线顶部右侧显示 Hz 值;
    // 横线 (1px) 在光标 y, 标签位于横线右端上侧显示 dB 值。
    // 标签矩形先算好传给刻度绘制 (DrawDbGrid/DrawFreqGrid): 与固定刻度重叠时
    // 隐藏那一个被重叠的刻度, 让位给准线读数。
    const bool inPlot =
        mHoverActive && mHoverX >= plot.L && mHoverX <= plot.R && mHoverY >= plot.T && mHoverY <= plot.B;
    IRECT hzSkip, dbSkip; // 默认空矩形 = 不跳过任何刻度
    char hzBuf[16] = "", dbBuf[16] = "";
    char noteBuf[8];   // 音高标签 (C4 等最近音名), inPlot 内计算
    IText hzText, dbText;
    IRECT pitchR, freqR; // 音高/频率标签绘制矩形, inPlot 内计算
    float xLine = 0.f, yLine = 0.f;
    if (inPlot) {
      xLine = mHoverX;
      yLine = mHoverY;

      // x -> 对数频率 (与 FreqNorm 反函数一致): 20Hz..20kHz
      const double hz = 20.0 * std::pow(1000.0, (double)(xLine - plot.L) / plot.W());
      FreqToNoteName(hz, noteBuf, sizeof(noteBuf));
      if (hz < 1000.0)
        std::snprintf(hzBuf, sizeof(hzBuf), "%.0f Hz", hz);
      else
        std::snprintf(hzBuf, sizeof(hzBuf), "%.2f kHz", hz / 1000.0);

      // y -> dB (mBottomDb..kTopDb)
      const float db = mBottomDb + (kTopDb - mBottomDb) * (plot.B - yLine) / plot.H();
      std::snprintf(dbBuf, sizeof(dbBuf), "%.1f dB", db);

      // Hz/音高标签: 与顶部频率刻度同布局同样式, 正常状态音高在准线左侧、频率在准线右侧,
      // 一侧放不下时折叠到另一侧 (见上方 noteBuf 注释)。矩形取文字实际范围, 供重叠判定。
      hzText = IText(14, COL_700(), kFontRegular, EAlign::Near, EVAlign::Top);
      // 先量出两标签的实际宽度 (矩形缩小为文字范围, 供两侧摆放)
      pitchR = IRECT(plot.L, plot.T + 2.f, plot.R, plot.T + 2.f + kLabelH);
      g.MeasureText(hzText, noteBuf, pitchR);
      freqR = IRECT(plot.L, plot.T + 2.f, plot.R, plot.T + 2.f + kLabelH);
      g.MeasureText(hzText, hzBuf, freqR);
      const float wP = pitchR.W(), wF = freqR.W();
      constexpr float kHzGap = 6.f; // 折叠并排时两标签间距
      const bool pitchFitsLeft = (xLine - 5.f - wP >= plot.L);
      const bool freqFitsRight = (xLine + 5.f + wF <= plot.R);
      if (pitchFitsLeft && freqFitsRight) {
        // 正常: 音高在准线左侧, 频率在准线右侧
        pitchR = IRECT(xLine - 5.f - wP, plot.T + 2.f, xLine - 5.f, plot.T + 2.f + kLabelH);
        freqR = IRECT(xLine + 5.f, plot.T + 2.f, xLine + 5.f + wF, plot.T + 2.f + kLabelH);
      } else if (!pitchFitsLeft) {
        // 音高放不进左侧: 避让到频率读数的右侧 (频率保持在准线右侧原位)
        freqR = IRECT(xLine + 5.f, plot.T + 2.f, xLine + 5.f + wF, plot.T + 2.f + kLabelH);
        pitchR = IRECT(freqR.R + kHzGap, plot.T + 2.f, freqR.R + kHzGap + wP, plot.T + 2.f + kLabelH);
      } else {
        // 频率放不进右侧: 避让到音高读数的右侧 (两标签并排在准线左侧)
        pitchR = IRECT(xLine - 5.f - wF - kHzGap - wP, plot.T + 2.f, xLine - 5.f - wF - kHzGap,
                       plot.T + 2.f + kLabelH);
        freqR = IRECT(xLine - 5.f - wF, plot.T + 2.f, xLine - 5.f, plot.T + 2.f + kLabelH);
      }
      hzSkip = pitchR.Union(freqR); // 与固定频率刻度避让: 两标签取并集矩形

      // dB 标签: 与右侧 dB 刻度同布局同样式 (文字在横线上方, 右对齐);
      // 顶部放不下时翻转到线下侧。
      dbText = IText(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Bottom);
      if (yLine - kLabelH - 1.f >= plot.T)
        dbSkip = IRECT(plot.R - 52.f, yLine - kLabelH - 1.f, plot.R - kTickRight, yLine - 1.f);
      else
        dbSkip = IRECT(plot.R - 52.f, yLine + 1.f, plot.R - kTickRight, yLine + 1.f + kLabelH);
    }

    DrawGridLayer(g, plot);
    DrawDbGrid(g, plot, dbSkip);
    DrawFreqGrid(g, plot, hzSkip);
    DrawVuLine(g, plot);
    DrawSpectrum(g, plot);

    if (inPlot) {
      // 准线颜色与刻度文字一致 (COL_700), 1px 细线
      g.DrawLine(COL_700(), xLine, plot.T, xLine, plot.B, nullptr, 1.f);
      g.DrawLine(COL_700(), plot.L, yLine, plot.R, yLine, nullptr, 1.f);
      g.DrawText(hzText, noteBuf, pitchR);
      g.DrawText(hzText, hzBuf, freqR);
      g.DrawText(dbText, dbBuf, dbSkip);
    }

    DrawLevelMeter(g, plot);
  }

private:
  // 静态背景网格绘制进离屏 Layer 缓存:
  // 内容只依赖 Range 底限与主题三值, 任一变化才重建, 平时每帧 1 次纹理 blit。
  // dB/频率刻度不在此层 (动态绘制, 供 hover 准线重叠隐藏), 见 DrawDbGrid/DrawFreqGrid。
  void DrawGridLayer(IGraphics &g, const IRECT &plot) {
    // 层位图四周外扩 1px (gridRect) 吸收引擎在纹理边缘的亚像素瑕疵;
    // 网格内部布局仍按 plot (与频谱曲线对齐), 仅最右列/最底行延伸到 gridRect 边缘,
    // 让位图边缘像素为网格色而非透明, 避免贴图时露出底色, 同时不产生视觉偏移。
    const IRECT gridRect = plot.GetPadded(1.f);
    const int hue = ThemeHue(), sat = ThemeSatMax(), mode = ThemeMode();
    if (!g.CheckLayer(mGridLayer) || mBottomDb != mGridBottomDb || hue != mGridHue || sat != mGridSat ||
        mode != mGridMode) {
      g.StartLayer(this, gridRect);
      DrawBackground(g, plot, gridRect);
      mGridLayer = g.EndLayer();
      mGridBottomDb = mBottomDb;
      mGridHue = hue;
      mGridSat = sat;
      mGridMode = mode;
    }
    g.DrawLayer(mGridLayer);
  }

  struct Pt {
    float x, y;
  };
  struct BandAcc {
    float max[3] = {0.f, 0.f, 0.f};
    float holdMax[3] = {0.f, 0.f, 0.f}; // 峰值保持: 同域聚合的各通道 hold 幅度 (FFT 模式 bin -> band)
    // 是否收到过 bin (与幅度无关)。FFT 模式的 band 是 bin 的聚合桶, 低频 band
    // 宽度可小于 bin 间距而完全无 bin; 无 bin 的空桶收集后用首个覆盖频带的值
    // 常值外推 (max/holdMax 同步), 有 bin 的照常收录
    char used[3] = {0, 0, 0};
  };

  // 频率(Hz) -> 归一化 x (0..1), 与 BandPass Freq 参数 (20..20000, ShapeExp) 一致
  static float FreqNorm(double hz) {
    return (float)(std::log(std::clamp(hz, 20.0, 20000.0) / 20.0) / std::log(20000.0 / 20.0));
  }

  // 频率 -> 最近音高
  static void FreqToNoteName(double hz, char *out, int outSize) {
    static const char *const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int nn = (int)std::lround(12.0 * std::log2(hz / 440.0)) + 69; // MIDI 音符号
    std::snprintf(out, outSize, "%s%d", kNoteNames[(nn % 12 + 12) % 12], nn / 12 - 1);
  }

  float XOf(const IRECT &plot, double f) const { return plot.L + FreqNorm(f) * plot.W(); }

  // 频率 × dB 二维色块网格背景:
  // 频率维度沿用原 DrawTrack 的分段渐变 (低频偏暗, 高频偏亮);
  // dB 维度每 20 dB 一横条 (底部深, 顶部浅);
  // 每个 cell 亮度取两维度平均后过 WarmGray, 饱和度由 SatForB 自动决定。
  // edge 为层位图边界 (比 plot 四周大 1px): 最右列/最底行延伸至 edge,
  // 内部位置仍按 plot 布局, 与频谱曲线坐标一致。
  void DrawBackground(IGraphics &g, const IRECT &plot, const IRECT &edge) {
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
      const float yLow = (r + 2 < nRows) ? dbToY(dbBounds[r + 1]) + 1.f : edge.B;
      const float vDb = vDbAt((dbBounds[r] + dbBounds[r + 1]) * 0.5f);
      for (size_t i = 0; i < nCols; ++i) {
        const auto &fc = freqCells[i];
        const float xR = (i + 1 < nCols) ? fc.xR + 1.f : edge.R;
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

    const float barL0 = plot.R;
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
    g.FillRect(COL_300(), bar);

    auto fillGrad = [&](float yTop, int alpha) {
      if (yTop >= bar.B)
        return;
      const IRECT active(bar.L, yTop, bar.R, bar.B);
      IPattern grad = IPattern::CreateLinearGradient(bar.L, plot.T, bar.L, plot.B);

      auto tOf = [&](float db) {
        return std::clamp((kTopDb - db) / (kTopDb - mBottomDb), 0.f, 1.f);
      };

      const float satScale = (ThemeSatMax() <= 30)
                                 ? ((float)ThemeSatMax() / 30.f)
                                 : (1.f + (float)(ThemeSatMax() - 30) / 55.f);

      auto meterColor = [&](int h, float baseS, float b) {
        const float s = std::clamp(baseS * satScale, 0.f, 1.f);
        const IColor c = HSBToIColor(h, s, b);
        return IColor(alpha, c.R, c.G, c.B);
      };

      grad.AddStop(meterColor(0, 0.80f, 0.90f), 0.f);          // +9 dB
      grad.AddStop(meterColor(4, 0.78f, 0.95f), tOf(0.f));     // 0 dB
      grad.AddStop(meterColor(32, 0.85f, 0.97f), tOf(-6.f));   // -6 dB
      grad.AddStop(meterColor(50, 0.80f, 0.89f), tOf(-14.f));  // -14 dB
      grad.AddStop(meterColor(140, 0.65f, 0.75f), tOf(-24.f)); // -24 dB
      grad.AddStop(meterColor(150, 0.75f, 0.66f), tOf(-48.f)); // -48 dB
      grad.AddStop(meterColor(156, 0.77f, 0.54f), 1.f);        // 底部

      g.PathClear();
      g.PathRect(active);
      g.PathFill(grad);
    };

    if (mMeterMode == 1) {
      const float yPeak = YOf(plot, val);
      const float yRms = YOf(plot, std::clamp(rmsVal, mBottomDb, kTopDb));
      fillGrad(yPeak, 90);
      fillGrad(yRms, 245);
    } else {
      fillGrad(YOf(plot, val), 255);
    }

    // 峰值保持线
    if (mHoldSec > 0.f && hold > mBottomDb) {
      const float yH = YOf(plot, std::clamp(hold, mBottomDb, top));
      g.FillRect(COL_900(), IRECT(bar.L, yH - 1.f, bar.R, yH + 1.f));
    }

    // over 指示 (仅 dBFS)
    if (mMeterMode == 1) {
      const float y0 = YOf(plot, 0.f);
      constexpr float kGap = 3.f;
      const IRECT ledR(bar.L, plot.T, bar.R, y0 - kGap);
      g.FillRect(COL_100(), IRECT(bar.L, plot.T, bar.R, y0));
      g.FillRect(COL_300(), ledR);
      if (over && y0 - kGap > plot.T + 1.f)
        g.FillRect(MeterOverLed(), ledR);
    }
  }

  // 每 20dB 一档的刻度文字: 绘制在频谱区域内部右侧靠边, 字号略小, 文字在线的上方。
  // skipRect 非空时, 与 hover 准线 dB 标签重叠的刻度隐藏 (让位给准线读数)。
  void DrawDbGrid(IGraphics &g, const IRECT &plot, const IRECT &skipRect) {
    if (mBottomDb >= 0.f)
      return;

    const int bottomDb = (int)mBottomDb;
    const IText t(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Bottom);

    for (int db = 0; db >= bottomDb; db -= 20) {
      // 最底部一条标签由 Range 循环按钮顶替 (按钮位于刻度列底部, 显示当前底部 dB 值), 跳过文字
      if (db == bottomDb)
        continue;

      const float y = plot.B - (float)(db - bottomDb) / (kTopDb - (float)bottomDb) * plot.H();

      // 文字在线的上边: 底部贴线 (留 1px), 右对齐到频谱区域右缘内侧 (0 也保持在上方)
      const IRECT labelR(plot.R - 52.f, y - kLabelH - 1.f, plot.R - kTickRight, y - 1.f);
      if (!skipRect.Empty() && labelR.Intersects(skipRect))
        continue;

      char buf[16];
      std::snprintf(buf, sizeof(buf), "%d", db);
      g.DrawText(t, buf, labelR);
    }
  }

  // VU 模式: 0 VU (-18 dBFS) 参考刻度线 (位于频谱区域内部右侧, 与刻度文字同区)。
  // 动态绘制 (不进入网格层缓存), 电平表模式切换即时生效, 无需重建层。
  void DrawVuLine(IGraphics &g, const IRECT &plot) {
    if (mMeterMode == 2 && mBottomDb <= -18.f) {
      const float y18 = YOf(plot, -18.f);
      g.FillRect(COL_700(), IRECT(plot.R - 52.f, y18 - 1.f, plot.R - kTickRight, y18 + 1.f));
    }
  }

  // 频率刻度: 20Hz / 100Hz / 1kHz / 10kHz, 位于频谱内部顶端, 文字在竖线右侧 (14px 与 dB 刻度一致)。
  // skipRect 非空时, 与 hover 准线 Hz 标签重叠的刻度隐藏 (按文字实际范围精确判定)。
  void DrawFreqGrid(IGraphics &g, const IRECT &plot, const IRECT &skipRect) {
    struct FreqLabel {
      double hz;
      const char *txt;
    };
    static const FreqLabel kLabels[] = {{20., "20Hz"}, {100., "100Hz"}, {1000., "1kHz"}, {10000., "10kHz"}};
    const IText t(14, COL_700(), kFontRegular, EAlign::Near, EVAlign::Top);
    for (const auto &lbl : kLabels) {
      const float x = XOf(plot, lbl.hz);
      // 文字在线的右边: 左缘贴线 (留 5px), 顶部贴频谱顶端
      const IRECT labelR(x + 5.f, plot.T + 2.f, plot.R, plot.T + 2.f + kLabelH);
      if (!skipRect.Empty()) {
        IRECT fit = labelR;
        g.MeasureText(t, lbl.txt, fit);
        if (fit.Intersects(skipRect))
          continue;
      }
      g.DrawText(t, lbl.txt, labelR);
    }
  }

  // 频谱峰值保持: 开关/时长与电平表 hold 共用 (mHoldSec 随电平表数据帧透传, 0 = 关,
  // ∞ 档为 1e9)。逐点跟踪平滑后幅度 (与显示同域: FFT 为 bin, 其余为 band):
  // 刷新峰值即清计时; 超时时长后按 20 dB/s 回落 (幅度域每秒 ×0.1), 下限为当前幅度。
  // 帧间 dt 取 steady_clock 实测, 首帧/挂起恢复后只采峰不回落。
  void UpdatePeakHold(int nVals) {
    if (mHoldSec <= 0.f) {
      if (mHoldWasActive) {
        ClearPeakHold();
        mHoldWasActive = false;
      }
      mHoldTpValid = false;
      return;
    }
    mHoldWasActive = true;

    const auto now = std::chrono::steady_clock::now();
    float dt = 0.f;
    if (mHoldTpValid)
      dt = std::clamp(std::chrono::duration<float>(now - mLastHoldTp).count(), 0.f, 0.25f);
    mLastHoldTp = now;
    mHoldTpValid = true;

    // 回落因子每帧一个; ∞ 档恒为 1, 计时照常累加但永不超时
    const float decay = (mHoldSec < 1e8f && dt > 0.f) ? std::pow(10.f, -dt) : 1.f;
    for (int c = 0; c < 3; ++c) {
      if (mHoldSpec[c].size() != (size_t)nVals) {
        mHoldSpec[c].assign(nVals, 0.f);
        mHoldAge[c].assign(nVals, 0.f);
        mHoldSignal = false;
      }
      for (int i = 0; i < nVals; ++i) {
        const float raw = mSpectrum[c][i];
        float &hold = mHoldSpec[c][i];
        if (raw > hold) {
          hold = raw;
          mHoldAge[c][i] = 0.f;
          if (raw > 1e-6f)
            mHoldSignal = true;
        } else if (decay < 1.f) {
          mHoldAge[c][i] += dt;
          if (mHoldAge[c][i] > mHoldSec && hold * decay < raw)
            hold = raw;
          else if (mHoldAge[c][i] > mHoldSec)
            hold *= decay;
        }
      }
    }
  }

  // hold 曲线单点值 (VQT/PBT/MR-FFT: band 域直接取): LR 显示取双通道 hold 较大者,
  // MERGE 用与显示曲线相同的 merge 公式 (PWR 功率和 / SUM 单声道和)。
  float HoldValAt(int b) const {
    const float hL = (mHoldSpec[0].size() > (size_t)b) ? mHoldSpec[0][b] : 0.f;
    const float hR = (mHoldSpec[1].size() > (size_t)b) ? mHoldSpec[1][b] : 0.f;
    if (mChanMode == 0)
      return (hL > hR) ? hL : hR;
    const float hSum = (mHoldSpec[2].size() > (size_t)b) ? mHoldSpec[2][b] : 0.f;
    return (mMergeAlgo == 0) ? std::sqrt(hL * hL + hR * hR) : hSum;
  }

  // hold 曲线单点值 (FFT: 256 band 聚合后): merge 公式同显示曲线
  float HoldBandVal(const BandAcc &acc) const {
    if (mChanMode == 0)
      return std::max(acc.holdMax[0], acc.holdMax[1]);
    return (mMergeAlgo == 0)
               ? std::sqrt(acc.holdMax[0] * acc.holdMax[0] + acc.holdMax[1] * acc.holdMax[1])
               : acc.holdMax[2];
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

    auto ampToY = [&](float amp) -> float {
      const float db =
          (amp > 1e-6f) ? std::clamp(orm::FastAmpToDb(amp, mBottomDb), mBottomDb, kTopDb) : mBottomDb;
      return plot.B - (db - mBottomDb) / (kTopDb - mBottomDb) * plot.H();
    };

    mHoldPts.clear();

    // VQT 模式: 数据 = band 幅度, 按 band 中心频率的原始对数位置直接绘制 (不做 256 band 聚合)
    if (mMode == 1) {
      mSpecPtsL.clear();
      mSpecPtsR.clear();
      mSpecPtsM.clear();
      const int nb = (int)mVQTFreqs.size();
      const int have = std::min(nb, (int)mSpectrum[0].size());
      for (int b = 0; b < have; ++b) {
        const float x = plot.L + mVQTFreqNorm[b] * plot.W();
        // 斜率: 每 band 幅度乘常数增益 (显示域变换, 平滑/hold 采集留在原始域;
        // 正增益与 max 聚合可交换, 显示曲线与 hold 曲线同步倾斜)
        const float g = SlopeGain(b);
        const float aL = mSpectrum[0][b] * g;
        const float aR = mSpectrum[1][b] * g;
        const float aSum = (mSpectrum[2].size() > (size_t)b) ? mSpectrum[2][b] * g : 0.f;
        const float yL = ampToY(aL);
        const float yR = ampToY(aR);
        mSpecPtsL.push_back({x, yL});
        mSpecPtsR.push_back({x, yR});

        const float aM = (mMergeAlgo == 0) ? std::sqrt(aL * aL + aR * aR) : aSum;
        mSpecPtsM.push_back({x, ampToY(aM)});
        mHoldPts.push_back({x, ampToY(HoldValAt(b) * g)});
      }

      if (mChanMode == 0) {
        DrawFill(g, plot, mSpecPtsL, cL, kGradientMinAlpha, kLayerTopAlpha, true, false);
        DrawFill(g, plot, mSpecPtsR, cR, kGradientMinAlpha, kLayerTopAlpha, true, true);
      } else {
        DrawFill(g, plot, mSpecPtsM, cO, kGradientMinAlpha, 255, true);
      }
      DrawHoldCurve(g, plot, true);
      return;
    }

    // PBT 模式: 数据 = 52/58/69 临界频带幅度, 按 PBT 中心频率绘制, 折线连接呈现经典嶙峋锯齿感
    if (mMode == 2) {
      mSpecPtsL.clear();
      mSpecPtsR.clear();
      mSpecPtsM.clear();
      const int nb = (int)mPBTFreqs.size();
      const int have = std::min(nb, (int)mSpectrum[0].size());
      for (int b = 0; b < have; ++b) {
        const float x = plot.L + mPBTFreqNorm[b] * plot.W();
        const float g = SlopeGain(b); // 斜率: 每 band 常数增益, 曲线与 hold 同步倾斜
        const float aL = mSpectrum[0][b] * g;
        const float aR = mSpectrum[1][b] * g;
        const float aSum = (mSpectrum[2].size() > (size_t)b) ? mSpectrum[2][b] * g : 0.f;
        const float yL = ampToY(aL);
        const float yR = ampToY(aR);
        mSpecPtsL.push_back({x, yL});
        mSpecPtsR.push_back({x, yR});

        const float aM = (mMergeAlgo == 0) ? std::sqrt(aL * aL + aR * aR) : aSum;
        mSpecPtsM.push_back({x, ampToY(aM)});
        mHoldPts.push_back({x, ampToY(HoldValAt(b) * g)});
      }

      if (mChanMode == 0) {
        DrawFill(g, plot, mSpecPtsL, cL, kGradientMinAlpha, kLayerTopAlpha, false, false);
        DrawFill(g, plot, mSpecPtsR, cR, kGradientMinAlpha, kLayerTopAlpha, false, true);
      } else {
        DrawFill(g, plot, mSpecPtsM, cO, kGradientMinAlpha, 255, false);
      }
      DrawHoldCurve(g, plot, false);
      return;
    }

    // MR-FFT 模式: 数据 = 八度子带小 FFT 各 band 幅度, 按对数频率平滑样条绘制
    if (mMode == 3) {
      mSpecPtsL.clear();
      mSpecPtsR.clear();
      mSpecPtsM.clear();
      const int nb = (int)mMRFFTFreqs.size();
      const int have = std::min(nb, (int)mSpectrum[0].size());
      for (int b = 0; b < have; ++b) {
        const float x = plot.L + mMRFFTFreqNorm[b] * plot.W();
        const float g = SlopeGain(b); // 斜率: 每 band 常数增益, 曲线与 hold 同步倾斜
        const float aL = mSpectrum[0][b] * g;
        const float aR = mSpectrum[1][b] * g;
        const float aSum = (mSpectrum[2].size() > (size_t)b) ? mSpectrum[2][b] * g : 0.f;
        const float yL = ampToY(aL);
        const float yR = ampToY(aR);
        mSpecPtsL.push_back({x, yL});
        mSpecPtsR.push_back({x, yR});

        const float aM = (mMergeAlgo == 0) ? std::sqrt(aL * aL + aR * aR) : aSum;
        mSpecPtsM.push_back({x, ampToY(aM)});
        mHoldPts.push_back({x, ampToY(HoldValAt(b) * g)});
      }

      if (mChanMode == 0) {
        DrawFill(g, plot, mSpecPtsL, cL, kGradientMinAlpha, kLayerTopAlpha, true, false);
        DrawFill(g, plot, mSpecPtsR, cR, kGradientMinAlpha, kLayerTopAlpha, true, true);
      } else {
        DrawFill(g, plot, mSpecPtsM, cO, kGradientMinAlpha, 255, true);
      }
      DrawHoldCurve(g, plot, true);
      return;
    }

    // RTA 模式: 数据 = 1/3, 1/4, 1/6 八度 IIR 滤波器组各频带 RMS 幅度, 按对数中心频率平滑贝塞尔曲线绘制
    if (mMode == 4) {
      mSpecPtsL.clear();
      mSpecPtsR.clear();
      mSpecPtsM.clear();
      const int nb = (int)mRTAFreqs.size();
      const int have = std::min(nb, (int)mSpectrum[0].size());
      for (int b = 0; b < have; ++b) {
        const float x = plot.L + mRTAFreqNorm[b] * plot.W();
        const float g = SlopeGain(b); // 斜率: 每 band 常数增益, 曲线与 hold 同步倾斜
        const float aL = mSpectrum[0][b] * g;
        const float aR = mSpectrum[1][b] * g;
        const float aSum = (mSpectrum[2].size() > (size_t)b) ? mSpectrum[2][b] * g : 0.f;
        const float yL = ampToY(aL);
        const float yR = ampToY(aR);
        mSpecPtsL.push_back({x, yL});
        mSpecPtsR.push_back({x, yR});

        const float aM = (mMergeAlgo == 0) ? std::sqrt(aL * aL + aR * aR) : aSum;
        mSpecPtsM.push_back({x, ampToY(aM)});
        mHoldPts.push_back({x, ampToY(HoldValAt(b) * g)});
      }

      if (mChanMode == 0) {
        DrawFill(g, plot, mSpecPtsL, cL, kGradientMinAlpha, kLayerTopAlpha, true, false);
        DrawFill(g, plot, mSpecPtsR, cR, kGradientMinAlpha, kLayerTopAlpha, true, true);
      } else {
        DrawFill(g, plot, mSpecPtsM, cO, kGradientMinAlpha, 255, true);
      }
      DrawHoldCurve(g, plot, true);
      return;
    }

    mSpecPtsL.clear();
    mSpecPtsR.clear();
    mSpecPtsM.clear();
    for (auto &acc : mBandAcc)
      acc = BandAcc{};
    mSubAnchor = BandAcc{};

    // bin -> band 映射查表 (预计算, 见 RebuildBinToBand), 避免每帧 2048*2 次 log2
    if ((int)mBinToBand.size() != mNumBins)
      RebuildBinToBand();
    const int nb = std::min(mNumBins, (int)mSpectrum[0].size());
    for (int i = 0; i < nb; ++i) {
      const int b = mBinToBand[i];
      if (b == kSubBand) { // 20Hz 下方锚点 (轴外位置, 只用于入场线斜率)
        for (int c = 0; c < 3; ++c) {
          if (i < (int)mSpectrum[c].size()) {
            const float amp = mSpectrum[c][i];
            if (amp > mSubAnchor.max[c])
              mSubAnchor.max[c] = amp;
            mSubAnchor.used[c] = 1;
          }
          if (i < (int)mHoldSpec[c].size()) {
            const float h = mHoldSpec[c][i];
            if (h > mSubAnchor.holdMax[c])
              mSubAnchor.holdMax[c] = h;
          }
        }
        continue;
      }
      if (b < 0)
        continue;
      for (int c = 0; c < 3; ++c) {
        if (i < (int)mSpectrum[c].size()) {
          const float amp = mSpectrum[c][i];
          if (amp > mBandAcc[b].max[c])
            mBandAcc[b].max[c] = amp;
          mBandAcc[b].used[c] = 1;
        }
        // hold 同域聚合 (hold 与 mSpectrum 同为 bin 域; 缺帧/未积累时按 0 处理)
        if (i < (int)mHoldSpec[c].size()) {
          const float h = mHoldSpec[c][i];
          if (h > mBandAcc[b].holdMax[c])
            mBandAcc[b].holdMax[c] = h;
        }
      }
    }

    // 低频空桶外推: 低于首个有 bin 的频带没有分析结果 (bin 间距 > band 宽度)。
    // 有 20Hz 下方锚点 (10-20Hz 桶) 时, 锚点与首个实测带之间按 dB-对数频率线性
    // 内插 —— 入场线带自然斜率 (锚点在显示轴外, 不画点); 无锚点 (如 LOW 档 bin
    // 间距 23Hz, 20Hz 下无 bin) 或锚点静音时退回首带常值延伸。
    // max/holdMax 同步外推, hold 曲线在外推段与主曲线保持一致
    const double logBand = (std::log2((double)kSpecFreqHi) - std::log2((double)kSpecFreqLo)) /
                           kSpectrumBands;
    for (int c = 0; c < 3; ++c) {
      int first = 0;
      while (first < kSpectrumBands && !mBandAcc[first].used[c])
        ++first;
      if (first >= kSpectrumBands)
        continue;
      const double fFirst = kSpecFreqLo * std::exp2(logBand * (first + 0.5));
      const float aFirst = mBandAcc[first].max[c];
      const float hFirst = mBandAcc[first].holdMax[c];
      const float aA = mSubAnchor.max[c];
      const bool haveAnchor = mSubAnchor.used[c] && aA > 1e-9f && aFirst > 1e-9f;
      for (int b = 0; b < first; ++b) {
        float ext = aFirst, extH = hFirst;
        if (haveAnchor) {
          const double fb = kSpecFreqLo * std::exp2(logBand * (b + 0.5));
          const double t = std::log2(fb / (double)kSpecAnchorHz) /
                           std::log2(fFirst / (double)kSpecAnchorHz);
          const float hA = (mSubAnchor.holdMax[c] > 1e-9f) ? mSubAnchor.holdMax[c] : hFirst;
          ext = aA * std::pow(aFirst / aA, (float)t);
          extH = hA * std::pow(hFirst / hA, (float)t);
        }
        mBandAcc[b].max[c] = ext;
        mBandAcc[b].holdMax[c] = extH;
        mBandAcc[b].used[c] = 1;
      }
    }

    for (int b = 0; b < kSpectrumBands; ++b) {
      const BandAcc &acc = mBandAcc[b];
      const float x = plot.L + mBandNormX[b] * plot.W();
      // 斜率: band 级乘常数增益 (bin 原始域聚合后施加; 正增益与 max 可交换),
      // 显示曲线与 hold 曲线 (同 band 域聚合) 同步倾斜
      const float g = SlopeGain(b);
      const float aL = acc.max[0] * g, aR = acc.max[1] * g, aSum = acc.max[2] * g;
      const float yL = ampToY(aL);
      const float yR = ampToY(aR);
      if (acc.used[0])
        mSpecPtsL.push_back({x, yL});
      if (acc.used[1])
        mSpecPtsR.push_back({x, yR});

      const float aM = (mMergeAlgo == 0) ? std::sqrt(aL * aL + aR * aR) : aSum;
      const bool usedM = (mMergeAlgo == 0) ? (acc.used[0] || acc.used[1]) : (acc.used[2] != 0);
      if (usedM)
        mSpecPtsM.push_back({x, ampToY(aM)});

      // hold 曲线点: 门控与显示点一致 (LR/PWR 看任一通道, SUM 看通道 2)
      const bool usedH =
          (mChanMode == 0 || mMergeAlgo == 0) ? (acc.used[0] || acc.used[1]) : (acc.used[2] != 0);
      if (usedH)
        mHoldPts.push_back({x, ampToY(HoldBandVal(acc) * g)});
    }

    if (mChanMode == 0) {
      DrawFill(g, plot, mSpecPtsL, cL, kGradientMinAlpha, kLayerTopAlpha, true, false);
      DrawFill(g, plot, mSpecPtsR, cR, kGradientMinAlpha, kLayerTopAlpha, true, true);
    } else {
      DrawFill(g, plot, mSpecPtsM, cO, kGradientMinAlpha, 255, true);
    }
    DrawHoldCurve(g, plot, true);
  }

  // 建开放曲线主路径 (右缘吸附 + 平滑/折线), 供填充与 hold 细线共用:
  // PathClear + MoveTo(pts[0]) + 曲线段; 不闭合不填充。
  void BuildCurvePath(IGraphics &g, const IRECT &plot, std::vector<Pt> &pts, bool smooth) {
    // 右边缘：末端已贴近右缘时吸附到 plot.R，保持"高频在实际最高频点处垂直收口"
    if (pts.back().x >= plot.R - plot.W() * 0.02f)
      pts.back().x = plot.R;

    g.PathClear();
    g.PathMoveTo(pts[0].x, pts[0].y);
    if (smooth && pts.size() > 3) {
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
  }

  void DrawFill(IGraphics &g, const IRECT &plot, std::vector<Pt> &pts, const IColor &color,
                int minAlpha = kGradientMinAlpha, int topAlpha = 255, bool smooth = true,
                bool complementary = false) {
    if (pts.size() < 2)
      return;

    BuildCurvePath(g, plot, pts, smooth);
    // 左边缘闭合：连到末端正下方与绘图区左下角。
    g.PathLineTo(pts.back().x, plot.B);
    g.PathLineTo(plot.L, plot.B);
    g.PathClose();

    // 渐变范围跟随曲线峰值，保证弱信号在底部也有足够的对比度
    float topY = plot.B;
    for (const Pt &p : pts)
      topY = std::min(topY, p.y);
    const IRECT gradRect(plot.L, topY, plot.R, plot.B);
    // 指数衰减渐变: 顶部接近实色, 按指数曲线快速向底部透明 (多 stops 近似)。
    // alpha 权重为固定序列 (GradW 首帧预计算), 每帧只做乘加与取整, 不再逐帧算 exp。
    const auto &gradW = GradW();
    IPattern fill = IPattern::CreateLinearGradient(gradRect, EDirection::Vertical);
    for (int i = 0; i < kGradientStops; ++i) {
      const float t = (float)i / (float)(kGradientStops - 1);
      const float a1 = std::clamp((float)minAlpha + (float)(topAlpha - minAlpha) * gradW[i], 0.f, 255.f);
      const float a = complementary ? ((255.f * a1) / (255.f + a1)) : a1;
      const int alpha = (int)std::lround(a);
      fill.AddStop(IColor(alpha, color.R, color.G, color.B), t);
    }
    g.PathFill(fill);
  }

  // 频谱峰值保持细线: 半透明极细线, 平滑策略与显示曲线一致 (PBT 折线, 其余贝塞尔);
  // hold 关闭 / 尚无有效峰值时不画。
  void DrawHoldCurve(IGraphics &g, const IRECT &plot, bool smooth) {
    if (mHoldSec <= 0.f || !mHoldSignal || mHoldPts.size() < 2)
      return;
    BuildCurvePath(g, plot, mHoldPts, smooth);
    const IColor c(kHoldLineAlpha, COL_900().R, COL_900().G, COL_900().B);
    g.PathStroke(IPattern(c), 1.f);
  }

  static constexpr int kGradientMinAlpha = 15; // L/R 实体填充底部最小不透明度
  static constexpr int kLayerTopAlpha = 200;   // L/R 底层(L)顶部不透明度; 顶层(R)动态映射为 112, 叠加总透明度为 224 / 88%
  static constexpr int kHoldLineAlpha = 75;    // 频谱峰值保持细线不透明度 (低 = 更浅更透)
  static constexpr int kGradientStops = 12;    // 渐变 stops 数 (指数近似精度)
  static constexpr float kGradientDecay = 3.5f;
  // 渐变 alpha 权重 exp(-kGradientDecay·t), 与绘制参数无关, 静态局部只在首帧初始化一次
  static const std::array<float, kGradientStops> &GradW() {
    static const std::array<float, kGradientStops> w = [] {
      std::array<float, kGradientStops> a{};
      for (int i = 0; i < kGradientStops; ++i) {
        const float t = (float)i / (float)(kGradientStops - 1);
        a[i] = std::exp(-kGradientDecay * t);
      }
      return a;
    }();
    return w;
  }
  static constexpr int kSpectrumBands = 256;
  static constexpr float kSpecFreqLo = 20.f;
  static constexpr float kSpecFreqHi = 20000.f;
  // 20Hz 下方虚拟锚点: 聚合 [10,20)Hz 的 bin 为一个位置 (显示轴外, 不单独画点),
  // 给低频空桶提供带自然斜率的入场线 (取代首带常值平延)。10Hz 以下 (含直流近旁) 不计。
  static constexpr float kSpecAnchorLo = 10.f;
  static constexpr float kSpecAnchorHz = 14.14214f; // sqrt(10·20), 锚点代表频率
  static constexpr int kSubBand = -2;               // mBinToBand 哨兵: 归入锚点桶
  static constexpr float kTopDb = 9.f;         // 显示范围顶部 (dBFS), 刻度线仍从 0 dB 开始
  static constexpr float kTickRight = 3.f;     // 刻度文字右缘距频谱区域右缘的边距
  static constexpr float kLabelH = 16.f;       // 14px 字行高 (刻度与 hover 准线标签共用)

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
      if (f > kSpecFreqHi)
        continue;
      if (f < kSpecFreqLo) {
        if (f >= kSpecAnchorLo)
          mBinToBand[i] = kSubBand; // 10-20Hz: 20Hz 下方锚点桶 (轴外)
        continue;
      }
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
  int mMode = 0;                   // 分析模式: 0=FFT, 1=VQT, 2=PBT, 3=MR-FFT, 4=RTA
  int mChanMode = 0;               // 声道显示模式: 0=L/R, 1=MERGE
  int mMergeAlgo = 0;              // 合并算法: 0=PWR 功率和, 1=SUM 单声道和
  std::vector<float> mVQTFreqs;     // VQT band 中心频率 (Hz), 由插件下发
  std::vector<float> mVQTFreqNorm;  // VQT band 频率归一化位置 (预计算, 与 mVQTFreqs 同步)
  std::vector<float> mPBTFreqs;     // PBT band 中心频率 (Hz), 由插件下发
  std::vector<float> mPBTFreqNorm;  // PBT band 频率归一化位置 (预计算, 与 mPBTFreqs 同步)
  std::vector<float> mMRFFTFreqs;   // MR-FFT band 中心频率 (Hz), 由插件下发
  std::vector<float> mMRFFTFreqNorm;// MR-FFT band 频率归一化位置 (预计算, 与 mMRFFTFreqs 同步)
  std::vector<float> mRTAFreqs;     // RTA band 中心频率 (Hz), 由插件下发
  std::vector<float> mRTAFreqNorm;  // RTA band 频率归一化位置 (预计算, 与 mRTAFreqs 同步)
  std::array<float, kSpectrumBands> mBandNormX{}; // FFT 256 band 频率归一化位置 (预计算)

  // ── 频谱斜率 (显示域变换) ────────────────────────────────────────────
  // 每显示值幅度乘常数增益 g(f) = 10^(S·log2(f/f_pivot)/20), S 为当前模式生效斜率
  // (FFT: 0/3/4.5 dB/oct; VQT/PBT/MR-FFT/RTA: -3/0/1.5, 由插件按模式档值下发)。
  // 增益为每 band 正常数, 与攻击/释放平滑及 hold 采集可交换, 故仅在绘制时施加;
  // 表在斜率/模式/band 表变化时重建 (与 RebuildBinToBand 同模式, 热路径只查表)。
  static constexpr float kSlopeRefHz = 632.45553f; // 支点 = 显示范围几何中心 sqrt(20·20000)
  float mSlopeDbPerOct = 0.f;   // 当前模式生效斜率 (dB/oct), 0 = 无倾斜
  std::vector<float> mSlopeGain; // 每显示值斜率增益 (FFT 256 band / 逐 band 引擎各 band)

  void RebuildSlopeGain() {
    std::vector<float> *freqs = nullptr;
    int n = 0;
    if (mMode == 0) {
      n = kSpectrumBands; // FFT: band 中心频率固定 (20..20k 对数均分, 见构造函数)
    } else {
      freqs = (mMode == 1) ? &mVQTFreqs : (mMode == 2) ? &mPBTFreqs : (mMode == 3) ? &mMRFFTFreqs : &mRTAFreqs;
      n = (int)freqs->size();
    }
    mSlopeGain.assign(n, 1.f);
    if (mSlopeDbPerOct == 0.f || n <= 0)
      return;
    // gain = 10^(S·log2(f/f0)/20) = exp2(S·log2(f/f0)·log2(10)/20)
    const float k = mSlopeDbPerOct * 0.16609640474f; // log2(10)/20
    const double logLo = std::log2(kSpecFreqLo);
    const double logBand = (std::log2(kSpecFreqHi) - logLo) / kSpectrumBands;
    for (int b = 0; b < n; ++b) {
      const float f = (mMode == 0) ? (float)(kSpecFreqLo * std::exp2(logBand * (b + 0.5)))
                                   : (*freqs)[b];
      mSlopeGain[b] = std::exp2f(k * orm::FastLog2(f / kSlopeRefHz));
    }
  }
  float SlopeGain(int b) const {
    return (b >= 0 && b < (int)mSlopeGain.size()) ? mSlopeGain[b] : 1.f;
  }
  float mAttackCoeff = 0.2f;
  float mReleaseCoeff = 0.9f;
  float mAttackSec = 0.05f; // 上升时间常数 (s), 由插件 Attack 参数下发
  float mReleaseSec = 0.2f; // 释放时间常数 (s), 由插件 Release 参数下发
  float mBottomDb = -100.f; // 频谱显示下限 (dBFS), 由插件 Range 参数下发 (-80/-100/-120); 初始与参数默认一致
  int mNumBins = 2048;
  double mSampleRate = 48000.0;

  std::vector<Pt> mSpecPtsL;    // 预分配: L 填充点
  std::vector<Pt> mSpecPtsR;    // 预分配: R 填充点
  std::vector<Pt> mSpecPtsM;    // 预分配: 合并声道 (L+R) 填充点
  std::vector<Pt> mHoldPts;     // hold 曲线绘制点 (每帧重建, 与显示点同 x)
  std::vector<BandAcc> mBandAcc; // 预分配: 每 band 双通道峰值
  BandAcc mSubAnchor;            // 20Hz 下方锚点桶 (kSubBand 哨兵聚合, 用后即弃)

  // 频谱峰值保持 (与 mSpectrum 同域同长: FFT 为 bin, 其余为 band); 开关/时长随电平表数据帧透传
  std::vector<float> mHoldSpec[3]; // 各通道 hold 幅度 (显示域, 非 dB)
  std::vector<float> mHoldAge[3];  // 各通道距上次刷新峰值的时间 (s)
  bool mHoldSignal = false;        // 已有有效峰值 (全零不画线)
  bool mHoldTpValid = false;       // 帧时间戳有效 (首帧不衰减)
  bool mHoldWasActive = false;     // hold 开关边沿检测 (关闭时清一次积累)
  std::chrono::steady_clock::time_point mLastHoldTp{};

  // hover 十字准线 (OnMouseOver/OnMouseOut 维护; 仅图形区内绘制)
  bool mHoverActive = false;
  float mHoverX = 0.f;
  float mHoverY = 0.f;

  // 静态网格离屏缓存; 状态哨兵初值保证首帧重建
  ILayerPtr mGridLayer;
  float mGridBottomDb = -1000.f;
  int mGridHue = -1;
  int mGridSat = -1;
  int mGridMode = -1;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
