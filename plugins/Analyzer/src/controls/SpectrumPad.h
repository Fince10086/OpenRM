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
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class SpectrumPad : public IControl {
public:
  // 尺寸必须与四个分析引擎的数据包严格一致 (ISender 整体拷贝，不一致会读取失败)
  using TDataPacket = std::array<float, 8192>;

  enum MsgTags {
    kMsgTagSampleRate = 1,
    kMsgTagFFTSize,
    kMsgTagRelease,
    kMsgTagReleaseMode, // 释放回落模式 (0: 对数域单极点, 1: 匀速 dB 速率)
    kMsgTagRange,
    kMsgTagSlope,       // 频谱斜率 dB/oct (当前模式生效值)
    kMsgTagMode,
    kMsgTagVQTBands,
    kMsgTagReset,       // 清空平滑缓冲 (γ/BPO/模式切换时下发)
    kMsgTagChanMode,    // 声道显示模式 (0: L/R, 1: MERGE)
    kMsgTagMergeAlgo,   // 合并算法 (0: PWR, 1: SUM)
    kMsgTagLevelMeter,  // 电平表数据 (LevelMeterUiData)
    kMsgTagLoudness,    // 响度数据 (LoudnessUiData)
    kMsgTagPBTBands,    // PBT 频带中心频率 (Hz)
    kMsgTagRTABands,    // RTA 频带中心频率 (Hz)
  };

  // 电平条点击动作 (Analyzer.cpp 绑定回调)
  enum EMeterClick {
    kClickResetPersist,   // dBTP 模式: 清持久锁存
    kClickResetMeterHold, // dBFS 模式: 清峰值保持
    kClickResetOver,      // dBFS 模式: 清过载
    kClickLoudScale,      // 响度窗顶刻度: 循环偏移 (+9/+18 LU)
    kClickLoudPreset,     // 响度目标刻度: 循环预设 (-9/-14/-23/-24 LUFS)
  };
  std::function<void(EMeterClick)> mMeterClickHandler;

  SpectrumPad(const IRECT &bounds) : IControl(bounds) {
    mSpecPtsL.reserve(kSpectrumBands);
    mSpecPtsR.reserve(kSpectrumBands);
    mSpecPtsM.reserve(kSpectrumBands);
    RebuildBinToBand();
    // 预计算 256 band 的对数频率归一化位置，绘制时只做乘加
    const double logLo = std::log2(kSpecFreqLo);
    const double logHi = std::log2(kSpecFreqHi);
    const double logBand = (logHi - logLo) / kSpectrumBands;
    for (int b = 0; b < kSpectrumBands; ++b) {
      const double fCenter = kSpecFreqLo * std::exp2(logBand * (b + 0.5));
      mBandNormX[b] = FreqNorm(fCenter);
    }
    mHoldPts.reserve(kSpectrumBands);
  }

  // 清空频谱峰值保持 (RESET 按钮联动; hold 关闭时自动调用一次)
  void ClearPeakHold() {
    for (int c = 0; c < 3; ++c) {
      mHoldSpec[c].assign(mHoldSpec[c].size(), -1000.f);
      mHoldAge[c].assign(mHoldAge[c].size(), 0.f);
    }
    mHoldSignal = false;
    SetDirty(false);
  }

  // 快捷键 P 切换绘制耗时 HUD
  void ToggleHud() {
    mHudOn = !mHudOn;
    SetDirty(false);
  }

  void OnMsgFromDelegate(int msgTag, int dataSize, const void *pData) override {
    IByteStream stream(pData, dataSize);

    if (msgTag == ISender<>::kUpdateMessage) {
      ++mDataPkts;
      ISenderData<3, TDataPacket> d;
      stream.Get(&d, 0);
      // FFT: 数据 = bins; VQT/PBT/RTA: 数据 = band 幅度
      const int nVals = (mMode == 0) ? std::max(mNumBins, 0)
                                     : (mMode == 1) ? (int)mVQTFreqs.size()
                                     : (mMode == 2) ? (int)mPBTFreqs.size()
                                                    : (int)mRTAFreqs.size();
      if (nVals <= 0)
        return;

      // hop 恒 1024 样本 (FFT overlap 规则保证，其余引擎 kHop 固定 1024)
      const double hop = 1024.0;
      const double updatePeriod = hop / std::max(mSampleRate, 1.0);
      mAttackCoeff = (float)std::exp(-updatePeriod / mAttackSec);
      mReleaseCoeff = (float)std::exp(-updatePeriod / mReleaseSec);
      // 匀速档: 每帧固定屏高比例下落 (2·τ 秒跨一屏)，与显示范围/斜率无关
      const float unifStepDb = (float)(updatePeriod / (2.0 * std::max(mReleaseSec, 1e-3f)) * (kTopDb - mBottomDb));

      const float a = mAttackCoeff, r = mReleaseCoeff;
      if (mMode == 0) {
        ProcessFFTBands(d, a, r, unifStepDb);
      } else {
        // VQT/PBT/RTA: 逐 band 显示域弹道
        const float *tilt = mSlopeDb.empty() ? nullptr : mSlopeDb.data();
        for (int c = 0; c < 3; ++c) {
          if (mSpectrum[c].size() != (size_t)nVals)
            mSpectrum[c].assign(nVals, -150.f);
          for (int i = 0; i < nVals; ++i) {
            const float rawDb =
                (d.vals[c][i] > 1e-30f) ? 6.02059991328f * orm::FastLog2(d.vals[c][i]) : -150.f;
            const float target = SmoothTarget(rawDb, tilt ? tilt[i] : 0.f);
            float aCoef = a;
            if (mMode == 2 && mPBTFreqs.size() == (size_t)nVals) {
              // PBT 物理起振 τ = 1/(π·bw): 窄带低频展现自然蓄力爬坡
              const float fc = mPBTFreqs[i];
              const float bw = (fc < 250.f)
                  ? ((i > 0 && mPBTFreqs[i] < 250.f) ? (mPBTFreqs[i] - mPBTFreqs[i - 1]) : 40.f)
                  : (fc * 0.1f);
              const float tauBand = std::max(mAttackSec, 1.f / (3.14159f * std::max(bw, 5.f)));
              aCoef = (float)std::exp(-updatePeriod / tauBand);
            }
            mSpectrum[c][i] = StepSmoothed(mSpectrum[c][i], target, aCoef, r, unifStepDb);
          }
        }
      }
      UpdatePeakHold();
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
      mReleaseSec = std::clamp(releaseSec, 0.01f, 10.f);
    } else if (msgTag == kMsgTagReleaseMode) {
      int mode;
      stream.Get(&mode, 0);
      mReleaseMode = std::clamp(mode, 0, 1);
    } else if (msgTag == kMsgTagRange) {
      float rangeDb;
      stream.Get(&rangeDb, 0);
      mBottomDb = -std::clamp(rangeDb, 80.f, 120.f);
    } else if (msgTag == kMsgTagSlope) {
      float slopeDb;
      stream.Get(&slopeDb, 0);
      mSlopeDbPerOct = slopeDb;
      RebuildSlopeGain();
      SetDirty(false);
    } else if (msgTag == kMsgTagMode) {
      int mode;
      stream.Get(&mode, 0);
      mMode = std::clamp(mode, 0, 3);
      RebuildSlopeGain(); // 斜率档值随模式变化
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
      RebuildSlopeGain();
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
      RebuildSlopeGain();
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
      RebuildSlopeGain();
      SetDirty(false);
    } else if (msgTag == kMsgTagLevelMeter) {
      ++mDataPkts;
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
      mVuHoldL = d.vuHoldL;
      mVuHoldR = d.vuHoldR;
      mPersistL = d.persistL;
      mPersistR = d.persistR;
      mHoldL = d.holdL;
      mHoldR = d.holdR;
      mHoldSec = d.holdSec;
      mMeterMode = std::clamp(d.mode, 0, 1);
      mOverL = d.overL != 0;
      mOverR = d.overR != 0;
      SetDirty(false);
    } else if (msgTag == kMsgTagLoudness) {
      ++mDataPkts;
      if (dataSize != (int)sizeof(LoudnessUiData))
        return;
      LoudnessUiData d;
      std::memcpy(&d, pData, sizeof(d));
      mMomentary = d.momentary;
      mShortTerm = d.shortTerm;
      mIntegrated = d.integrated;
      mRange = d.range;
      mLraMin = d.lraMin;
      mLraMax = d.lraMax;
      mLraValid = d.lraValid != 0;
      mTarget = d.target;
      mScaleOff = (float)d.scaleOff;
      SetDirty(false);
    } else if (msgTag == kMsgTagReset) {
      ++mDataPkts;
      for (int c = 0; c < 3; ++c)
        mSpectrum[c].assign(mSpectrum[c].size(), -150.f);
      ClearPeakHold();
      SetDirty(false);
    }
  }

  // 点击优先级: 响度刻度按钮 > L/R 条区域 (dBTP 清持久锁存; dBFS 顶部清过载/条体清峰值保持)
  void OnMouseDown(float x, float y, const IMouseMod &mod) override {
    if (!mMeterClickHandler)
      return;
    {
      const IRECT approxPlot(mRECT.L, mRECT.T, mRECT.R - kMeterStripW, mRECT.B);
      const IRECT scaleBtnR = LoudScaleTickBtnRect(approxPlot);
      if (x >= scaleBtnR.L && x <= scaleBtnR.R && y >= scaleBtnR.T && y <= scaleBtnR.B) {
        mMeterClickHandler(kClickLoudScale);
        return;
      }
      if (mTarget > -100.f) {
        const IRECT presetBtnR = LoudPresetTickBtnRect(approxPlot);
        if (x >= presetBtnR.L && x <= presetBtnR.R && y >= presetBtnR.T && y <= presetBtnR.B) {
          mMeterClickHandler(kClickLoudPreset);
          return;
        }
      }
    }
    const float totalW = kMeterStripW;
    const float barL0 = mRECT.R - totalW;
    const float barR = barL0 + 2.f * kGainBarW;
    if (x < barL0 || x > barR || y < mRECT.T || y > mRECT.B)
      return;
    if (mMeterMode == 0) {
      mMeterClickHandler(kClickResetPersist);
    } else {
      const float y0 = YOf(mRECT, 0.f);
      if (y < y0 - 3.f)
        mMeterClickHandler(kClickResetOver);
      else
        mMeterClickHandler(kClickResetMeterHold);
    }
  }

  // hover 十字准线: 记录位置并重绘，离开时清除
  void OnMouseOver(float x, float y, const IMouseMod &mod) override {
    if (!mHoverActive || mHoverX != x || mHoverY != y) {
      mHoverX = x;
      mHoverY = y;
      mHoverActive = true;
      SetDirty(false);
    }
  }

  void OnMouseOut() override {
    if (mHoverActive) {
      mHoverActive = false;
      SetDirty(false);
    }
  }

  // 绘制耗时采样外壳 + HUD; 实际内容在 DrawContent
  void Draw(IGraphics &g) override {
    const auto perfT0 = std::chrono::steady_clock::now();
    DrawContent(g);
    PerfSample(std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - perfT0).count());
    if (mHudOn)
      DrawHud(g);
  }

private:
  void DrawContent(IGraphics &g) {
    g.FillRect(COL_100(), mRECT);
    // 图形区左对齐，右侧让出电平条带；物理像素对齐避免亚像素白边
    const IRECT plot = mRECT.GetReducedFromRight(kMeterStripW)
                           .GetPixelAligned(g.GetScreenScale() * g.GetDrawScale());

    // hover 准线分三个刻度域: 频谱+L/R 条 (共用 dB) | VU 表 | 响度条；
    // 竖线+频率读数只在频谱区，dBFS 模式 L/R 条顶部 LED 区不显示准线
    const float zoneLr = LrZoneR(plot);
    const float zoneVu = VuZoneR(plot);
    const bool ledArea = (mMeterMode == 1) && (mHoverX > plot.R) &&
                         (mHoverX <= zoneLr) && (mHoverY < YOf(plot, 0.f) - 3.f);
    const bool inStrip = mHoverActive && !ledArea && mHoverX >= plot.L && mHoverX <= mRECT.R &&
                         mHoverY >= plot.T && mHoverY <= plot.B;
    const bool inSpectrum = inStrip && mHoverX <= plot.R;
    const bool inZone0 = inStrip && mHoverX <= zoneLr;
    const bool inZone1 = inStrip && mHoverX > LrZoneR(plot) + kVuScaleW && mHoverX <= zoneVu;
    const bool inZone2 = inStrip && mHoverX > VuZoneR(plot) + kLufsScaleW;
    IRECT hzSkip, dbSkip, vuSkip, lufsSkip;
    char hzBuf[16] = "", dbBuf[16] = "";
    char vuBuf[16] = "", lufsBuf[16] = "";
    char noteBuf[8];
    IText hzText, dbText, vuText, lufsText;
    IRECT pitchR, freqR;
    IRECT vuBox, lufsBox;
    IRECT vuChip, lufsChip;
    float xLine = 0.f, yLine = 0.f;
    if (inStrip) {
      xLine = mHoverX;
      yLine = mHoverY;

      if (inSpectrum) {
        const double hz = 20.0 * std::pow(1000.0, (double)(xLine - plot.L) / plot.W());
        FreqToNoteName(hz, noteBuf, sizeof(noteBuf));
        if (hz < 1000.0)
          std::snprintf(hzBuf, sizeof(hzBuf), "%.0f Hz", hz);
        else
          std::snprintf(hzBuf, sizeof(hzBuf), "%.2f kHz", hz / 1000.0);
      }

      if (inZone0) {
        const float db = mBottomDb + (kTopDb - mBottomDb) * (plot.B - yLine) / plot.H();
        std::snprintf(dbBuf, sizeof(dbBuf), "%.1f dB", db);
      } else if (inZone1) {
        const float vu = kVuBottomVU + (plot.B - yLine) / plot.H() * (kVuTopDb - kVuBottomVU);
        std::snprintf(vuBuf, sizeof(vuBuf), "%+.1f", vu);
        vuText = IText(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Bottom);
        const float scaleVu = LrZoneR(plot);
        if (yLine - kLabelH - 1.f >= plot.T)
          vuBox = IRECT(scaleVu, yLine - kLabelH - 1.f, scaleVu + kVuScaleW - kTickRight, yLine - 1.f);
        else
          vuBox = IRECT(scaleVu, yLine + 1.f, scaleVu + kVuScaleW - kTickRight, yLine + 1.f + kLabelH);
        g.MeasureText(vuText, vuBuf, vuBox);
        vuChip = vuBox.GetPadded(2.f);
        vuSkip = vuChip;
      } else {
        float topL, botL;
        LoudWindow(topL, botL);
        const float lufs = botL + (plot.B - yLine) / plot.H() * (topL - botL);
        std::snprintf(lufsBuf, sizeof(lufsBuf), "%+.1f", lufs);
        lufsText = IText(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Bottom);
        const float scaleLufs = VuZoneR(plot);
        if (yLine - kLabelH - 1.f >= plot.T)
          lufsBox = IRECT(scaleLufs, yLine - kLabelH - 1.f, scaleLufs + kLufsScaleW - kTickRight,
                          yLine - 1.f);
        else
          lufsBox = IRECT(scaleLufs, yLine + 1.f, scaleLufs + kLufsScaleW - kTickRight,
                          yLine + 1.f + kLabelH);
        g.MeasureText(lufsText, lufsBuf, lufsBox);
        lufsChip = lufsBox.GetPadded(2.f);
        // LUFS 读数与固定刻度重叠时纵向平移让开，使刻度按钮始终可见可点
        {
          const float gap = 3.f;
          const IRECT btnObstacles[3] = {LoudScaleTickBtnRect(plot),
                                         (mTarget > -100.f) ? LoudPresetTickBtnRect(plot)
                                                            : IRECT(),
                                         LufsTickRect(plot, botL)};
          for (const IRECT &btn : btnObstacles) {
            if (btn.Empty() || !lufsChip.Intersects(btn))
              continue;
            float dy = (btn.T - gap) - lufsChip.B;
            if (lufsChip.T + dy < plot.T) {
              dy = (btn.B + gap) - lufsChip.T;
              if (lufsChip.B + dy > plot.B)
                continue;
            }
            lufsBox.T += dy;
            lufsBox.B += dy;
            lufsChip.T += dy;
            lufsChip.B += dy;
          }
        }
        lufsSkip = lufsChip;
      }

      if (inSpectrum) {
        hzText = IText(14, COL_700(), kFontRegular, EAlign::Near, EVAlign::Top);
        pitchR = IRECT(plot.L, plot.T + 2.f, plot.R, plot.T + 2.f + kLabelH);
        g.MeasureText(hzText, noteBuf, pitchR);
        freqR = IRECT(plot.L, plot.T + 2.f, plot.R, plot.T + 2.f + kLabelH);
        g.MeasureText(hzText, hzBuf, freqR);
        const float wP = pitchR.W(), wF = freqR.W();
        constexpr float kHzGap = 6.f;
        const bool pitchFitsLeft = (xLine - 5.f - wP >= plot.L);
        const bool freqFitsRight = (xLine + 5.f + wF <= plot.R);
        if (pitchFitsLeft && freqFitsRight) {
          pitchR = IRECT(xLine - 5.f - wP, plot.T + 2.f, xLine - 5.f, plot.T + 2.f + kLabelH);
          freqR = IRECT(xLine + 5.f, plot.T + 2.f, xLine + 5.f + wF, plot.T + 2.f + kLabelH);
        } else if (!pitchFitsLeft) {
          freqR = IRECT(xLine + 5.f, plot.T + 2.f, xLine + 5.f + wF, plot.T + 2.f + kLabelH);
          pitchR = IRECT(freqR.R + kHzGap, plot.T + 2.f, freqR.R + kHzGap + wP, plot.T + 2.f + kLabelH);
        } else {
          pitchR = IRECT(xLine - 5.f - wF - kHzGap - wP, plot.T + 2.f, xLine - 5.f - wF - kHzGap,
                         plot.T + 2.f + kLabelH);
          freqR = IRECT(xLine - 5.f - wF, plot.T + 2.f, xLine - 5.f, plot.T + 2.f + kLabelH);
        }
        hzSkip = pitchR.Union(freqR);
      }

      if (inZone0) {
        dbText = IText(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Bottom);
        if (yLine - kLabelH - 1.f >= plot.T)
          dbSkip = IRECT(plot.R - 52.f, yLine - kLabelH - 1.f, plot.R - kTickRight, yLine - 1.f);
        else
          dbSkip = IRECT(plot.R - 52.f, yLine + 1.f, plot.R - kTickRight, yLine + 1.f + kLabelH);
      }
    }

    DrawGridLayer(g, plot);
    DrawDbGrid(g, plot, dbSkip);
    DrawFreqGrid(g, plot, hzSkip);
    DrawSpectrum(g, plot);
    DrawLevelMeter(g, plot, vuSkip, lufsSkip);
    DrawLraBracket(g, plot);

    if (inZone0) {
      if (inSpectrum) {
        g.DrawLine(COL_700(), xLine, plot.T, xLine, plot.B, nullptr, 1.f);
        g.DrawText(hzText, noteBuf, pitchR);
        g.DrawText(hzText, hzBuf, freqR);
      }
      g.DrawLine(COL_700(), plot.L, yLine, zoneLr, yLine, nullptr, 1.f);
      g.DrawText(dbText, dbBuf, dbSkip);
    } else if (inZone1) {
      g.DrawLine(COL_700(), zoneLr, yLine, zoneVu, yLine, nullptr, 1.f);
      g.DrawText(vuText, vuBuf, vuBox);
    } else if (inZone2) {
      g.DrawLine(COL_700(), zoneVu, yLine, mRECT.R, yLine, nullptr, 1.f);
      g.DrawText(lufsText, lufsBuf, lufsBox);
    }
  }

private:
  // ---- 绘制耗时采样与 HUD (快捷键 P 开关) ----

  // 采样一次 pad 绘制耗时; data/hover 帧分类 = 自上次绘制以来是否收到过数据包
  void PerfSample(float ms) {
    mHudMs[mHudPos] = ms;
    mHudIsData[mHudPos] = (mDataPkts > 0) ? 1 : 0;
    mDataPkts = 0;
    mHudT[mHudPos] =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    mHudPos = (mHudPos + 1) % kHudN;
    if (mHudFill < kHudN)
      ++mHudFill;
  }

  // 绘制耗时 HUD (绘图区左下角，最近 120 次绘制 ~2s)
  void DrawHud(IGraphics &g) {
    if (mHudFill <= 0)
      return;
    const double now =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    float sum = 0.f, sumD = 0.f, sumH = 0.f, mx = 0.f;
    int nD = 0, nH = 0, nD1s = 0, nH1s = 0, nAll1s = 0;
    for (int i = 0; i < mHudFill; ++i) {
      const float v = mHudMs[i];
      sum += v;
      if (v > mx)
        mx = v;
      if (mHudIsData[i]) {
        sumD += v;
        ++nD;
      } else {
        sumH += v;
        ++nH;
      }
      if (now - mHudT[i] <= 1.0) {
        ++nAll1s;
        if (mHudIsData[i])
          ++nD1s;
        else
          ++nH1s;
      }
    }
    const float avg = sum / mHudFill;
    const float avgD = (nD > 0) ? sumD / nD : 0.f;
    const float avgH = (nH > 0) ? sumH / nH : 0.f;

    char l1[96], l2[96];
    std::snprintf(l1, sizeof(l1), "pad %.2f ms avg / %.2f max / %d draw/s", avg, mx, nAll1s);
    std::snprintf(l2, sizeof(l2), "data %.2f ms x%d/s | hover %.2f ms x%d/s", avgD, nD1s, avgH, nH1s);

    const IText t(11, COL_700(), kFontRegular, EAlign::Near, EVAlign::Bottom);
    const IRECT plot = mRECT.GetReducedFromRight(kMeterStripW);
    IRECT r1, r2;
    g.MeasureText(t, l1, r1);
    g.MeasureText(t, l2, r2);
    const float wBox = std::max(r1.W(), r2.W()) + 8.f;
    const IRECT box(plot.L + 4.f, plot.B - 38.f, plot.L + 4.f + wBox, plot.B - 4.f);
    g.FillRect(IColor(190, COL_100().R, COL_100().G, COL_100().B), box);
    g.DrawText(t, l1, IRECT(box.L + 4.f, box.T + 2.f, box.R, box.T + 19.f));
    g.DrawText(t, l2, IRECT(box.L + 4.f, box.T + 19.f, box.R, box.B - 2.f));
  }

  // 静态背景网格离屏缓存: 只依赖 Range 底限与主题三值，任一变化才重建
  void DrawGridLayer(IGraphics &g, const IRECT &plot) {
    // 层位图四周外扩 1px 吸收纹理边缘亚像素瑕疵
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

  // 频率(Hz) -> 归一化 x (0..1)，与 BandPass Freq 参数一致
  static float FreqNorm(double hz) {
    return (float)(std::log(std::clamp(hz, 20.0, 20000.0) / 20.0) / std::log(20000.0 / 20.0));
  }

  // 频率 -> 最近音高
  static void FreqToNoteName(double hz, char *out, int outSize) {
    static const char *const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int nn = (int)std::lround(12.0 * std::log2(hz / 440.0)) + 69;
    std::snprintf(out, outSize, "%s%d", kNoteNames[(nn % 12 + 12) % 12], nn / 12 - 1);
  }

  float XOf(const IRECT &plot, double f) const { return plot.L + FreqNorm(f) * plot.W(); }

  // 频率×dB 二维色块网格背景; edge 为层位图边界 (比 plot 大 1px)
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

    std::vector<float> dbBounds;
    dbBounds.push_back(kTopDb);
    for (int db = 0; db > (int)mBottomDb; db -= 20)
      dbBounds.push_back((float)db);
    dbBounds.push_back(mBottomDb);

    auto dbToY = [&](float db) -> float {
      return plot.B - (db - mBottomDb) / (kTopDb - mBottomDb) * plot.H();
    };

    constexpr float kVDbTop = 245.f;
    constexpr float kVDbBottom = 172.f;
    auto vDbAt = [&](float db) -> float {
      const float t = (kTopDb - db) / (kTopDb - mBottomDb);
      return kVDbTop + (kVDbBottom - kVDbTop) * t;
    };

    // 相邻 cell 各向右侧/下侧重叠 1px，消除抗锯齿亚像素间隙
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

  // 电平表满刻度: dBTP +6 dB / dBFS 0 dBFS；刻度网格仍固定到 kTopDb，顶部空出放 over LED
  float MeterTopDb() const {
    return (mMeterMode == 1) ? 0.f : kTopDb;
  }

  float YOf(const IRECT &plot, float db) const {
    return plot.B - (db - mBottomDb) / (kTopDb - mBottomDb) * plot.H();
  }

  // 电平条带分三个刻度域: 频谱+L/R 条 (共用 dB) | VU 表 | 响度条
  float LrZoneR(const IRECT &plot) const { return plot.R + 2.f * kGainBarW; }
  float VuZoneR(const IRECT &plot) const { return LrZoneR(plot) + kVuScaleW + 2.f * kVuBarW; }

  IRECT VuTickRect(const IRECT &plot, float vu) const {
    const float scaleL = LrZoneR(plot);
    if (vu >= kVuTopDb)
      return IRECT(scaleL, plot.T + 1.f, scaleL + kVuScaleW - kTickRight, plot.T + 1.f + kLabelH);
    const float y = plot.B - (vu - kVuBottomVU) / (kVuTopDb - kVuBottomVU) * plot.H();
    return IRECT(scaleL, y - kLabelH - 1.f, scaleL + kVuScaleW - kTickRight, y - 1.f);
  }

  // 响度条目标锚定刻度窗: 顶/底 LUFS (目标无效退回固定 -60..0)
  void LoudWindow(float &topL, float &botL) const {
    if (mTarget > -100.f) {
      topL = mTarget + mScaleOff;
      botL = mTarget - 2.f * mScaleOff;
    } else {
      topL = 0.f;
      botL = -60.f;
    }
  }
  float LoudYOf(const IRECT &plot, float lufs) const {
    float topL, botL;
    LoudWindow(topL, botL);
    return plot.B - std::clamp((lufs - botL) / (topL - botL), 0.f, 1.f) * plot.H();
  }

  IRECT LufsTickRect(const IRECT &plot, float lufs) const {
    const float scaleL = VuZoneR(plot);
    float topL, botL;
    LoudWindow(topL, botL);
    if (lufs >= topL)
      return IRECT(scaleL, plot.T + 1.f, scaleL + kLufsScaleW - kTickRight, plot.T + 1.f + kLabelH);
    const float y = LoudYOf(plot, lufs);
    return IRECT(scaleL, y - kLabelH - 1.f, scaleL + kLufsScaleW - kTickRight, y - 1.f);
  }

  // 响度刻度按钮 (窗顶值 / 目标值): 文字与普通刻度一致，hover 时垫半透明遮罩
  static IRECT LoudTickBtnRect(const IRECT &labelR) {
    return IRECT(labelR.L, labelR.T - 1.f, labelR.R + 1.f, labelR.B + 1.f);
  }
  IRECT LoudScaleTickBtnRect(const IRECT &plot) const {
    float topL, botL;
    LoudWindow(topL, botL);
    return LoudTickBtnRect(LufsTickRect(plot, topL));
  }
  IRECT LoudPresetTickBtnRect(const IRECT &plot) const {
    return LoudTickBtnRect(LufsTickRect(plot, mTarget));
  }

  // L/R 双电平表 + 独立 VU 表 + 响度条
  void DrawLevelMeter(IGraphics &g, const IRECT &plot, const IRECT &vuSkip, const IRECT &lufsSkip) {
    IColor cL, cR, cM;
    GetChannelColors(cL, cR, cM);

    const float barL0 = plot.R;
    const IRECT barL(barL0, plot.T, barL0 + kGainBarW, plot.B);
    const IRECT barR(barL.R, plot.T, barL.R + kGainBarW, plot.B);

    DrawMeterBar(g, plot, barL, cL, 0);
    DrawMeterBar(g, plot, barR, cR, 1);
    DrawVuScale(g, plot, vuSkip);
    DrawVuBar(g, plot);
    DrawLoudBars(g, plot, lufsSkip);
  }

  // VU 表刻度文字: -20/-10/0/+3，画在 L/R 条与 VU 表之间；hover 读数重叠时隐藏让位
  void DrawVuScale(IGraphics &g, const IRECT &plot, const IRECT &skipRect) {
    const IText t(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Bottom);
    struct VuTick {
      int vu;
      const char *txt;
    };
    static const VuTick kTicks[] = {{-20, "-20"}, {-10, "-10"}, {0, "0"}, {3, "+3"}};
    for (const auto &tk : kTicks) {
      const IRECT labelR = VuTickRect(plot, (float)tk.vu);
      if (!skipRect.Empty() && labelR.Intersects(skipRect))
        continue;
      g.DrawText(t, tk.txt, labelR);
    }
  }

  // 独立 VU 表双条 (L/R): 0 VU = -18 dBFS，刻度 -20..+3 VU；两段语义色渐变
  void DrawVuBar(IGraphics &g, const IRECT &plot) {
    const float scaleL = plot.R + 2.f * kGainBarW;
    const IRECT barL(scaleL + kVuScaleW, plot.T, scaleL + kVuScaleW + kVuBarW, plot.B);
    const IRECT barR(barL.R, plot.T, barL.R + kVuBarW, plot.B);

    const float satScale = (ThemeSatMax() <= 30)
                               ? ((float)ThemeSatMax() / 30.f)
                               : (1.f + (float)(ThemeSatMax() - 30) / 55.f);

    auto tOfV = [](float vu) {
      return std::clamp((kVuTopDb - vu) / (kVuTopDb - kVuBottomVU), 0.f, 1.f);
    };
    auto yOfV = [&](float vu, const IRECT &bar) {
      return bar.B - (vu - kVuBottomVU) / (kVuTopDb - kVuBottomVU) * bar.H();
    };

    struct MeterStop {
      float t;
      int h;
      float s;
      float b;
    };
    const MeterStop stops[] = {
      {0.f, 0, 0.72f, 0.93f},
      {tOfV(-3.f), 120, 0.55f, 0.78f},
      {1.f, 150, 0.62f, 0.40f},
    };
    const int nStops = (int)(sizeof(stops) / sizeof(stops[0]));

    auto meterColor = [&](const MeterStop &st, int alpha) {
      const float s = std::clamp(st.s * satScale, 0.f, 1.f);
      const IColor c = HSBToIColor(st.h, s, st.b);
      return IColor(alpha, c.R, c.G, c.B);
    };

    auto colorAtT = [&](float t, int alpha) {
      for (int i = 0; i + 1 < nStops; ++i) {
        if (t <= stops[i + 1].t) {
          const float span = stops[i + 1].t - stops[i].t;
          const float f = (span > 0.f) ? (t - stops[i].t) / span : 0.f;
          const IColor a = meterColor(stops[i], alpha);
          const IColor b = meterColor(stops[i + 1], alpha);
          return IColor(alpha, (int)(a.R + (b.R - a.R) * f + 0.5f),
                              (int)(a.G + (b.G - a.G) * f + 0.5f),
                              (int)(a.B + (b.B - a.B) * f + 0.5f));
        }
      }
      return meterColor(stops[nStops - 1], alpha);
    };

    auto meterColorAt = [&](float vu, int alpha) { return colorAtT(tOfV(vu), alpha); };

    auto drawBar = [&](const IRECT &bar, float vuDb, float vuHold) {
      g.FillRect(COL_300(), bar);

      // NanoVG 后端不支持多 stop 渐变，按 stop 分段用 2-stop 渐变，段间重叠 1 设备像素
      auto fillGrad = [&](float yTop, int alpha) {
        if (yTop >= bar.B)
          return;
        const float ov = 1.f / std::max(1.f, g.GetScreenScale() * g.GetDrawScale());
        for (int i = 0; i + 1 < nStops; ++i) {
          const float yA = plot.T + stops[i].t * plot.H();
          const float yB = plot.T + stops[i + 1].t * plot.H();
          if (yB <= yTop)
            continue;
          const float rT = std::max(yA - ov, yTop);
          const float rB = std::min(yB + ov, bar.B);
          if (rB - rT <= 0.f)
            continue;
          IPattern grad = IPattern::CreateLinearGradient(bar.L, yA, bar.L, yB);
          grad.AddStop(meterColor(stops[i], alpha), 0.f);
          grad.AddStop(meterColor(stops[i + 1], alpha), 1.f);
          g.PathClear();
          g.PathRect(IRECT(bar.L, rT, bar.R, rB));
          g.PathFill(grad);
        }
      };

      // vuDb 为 dBFS 域 (0 VU = -18 dBFS)，转 VU 域后映射
      const float vu = vuDb + 18.f;
      const float yTop = yOfV(std::clamp(vu, kVuBottomVU, kVuTopDb), bar);
      fillGrad(yTop, 255);

      if (mHoldSec > 0.f && vuHold > -900.f) {
        const float vh = vuHold + 18.f;
        if (vh > kVuBottomVU) {
          const float yH = yOfV(std::clamp(vh, kVuBottomVU, kVuTopDb), bar);
          g.FillRect(meterColorAt(vh, 255), IRECT(bar.L, yH - 1.f, bar.R, yH + 1.f));
        }
      }
    };
    drawBar(barL, mVuL, mVuHoldL);
    drawBar(barR, mVuR, mVuHoldR);

    // "VU" 水印: 双条底部，空轨深灰、有渐变浅灰
    const float vuMaxDb = (mVuL > mVuR) ? mVuL : mVuR;
    const IColor vuLblCol = (vuMaxDb + 18.f > kVuBottomVU) ? COL_300() : COL_700();
    const IText vuLbl(11.f, vuLblCol, kFontSemiBold, EAlign::Center, EVAlign::Middle);
    g.DrawText(vuLbl, "VU", IRECT(barL.L, plot.B - 19.f, barR.R, plot.B));
  }

  // 语义色降饱和 (固定 RGB 安全色随主题饱和档位; 黑白主题变灰阶)
  static float MeterSatScale2() {
    const int sat = ThemeSatMax();
    return (sat <= 30) ? (float)sat / 30.f : (1.f + (float)(sat - 30) / 55.f);
  }
  static IColor SemColor(IColor c) {
    const float m = std::clamp(MeterSatScale2(), 0.f, 1.f);
    const float lum = 0.299f * c.R + 0.587f * c.G + 0.114f * c.B;
    return IColor(c.A, (int)std::lround(c.R * m + lum * (1.f - m)),
                       (int)std::lround(c.G * m + lum * (1.f - m)),
                       (int)std::lround(c.B * m + lum * (1.f - m)));
  }

  // 响度三条 (LUFS): 排列 I(2倍宽) | S | M；目标锚定刻度，语义色按目标相对分三段
  void DrawLoudBars(IGraphics &g, const IRECT &plot, const IRECT &skipRect) {
    const float scaleL = VuZoneR(plot);
    const float bar0L = scaleL + kLufsScaleW;
    const IRECT barI(bar0L, plot.T, bar0L + 2.f * kLoudBarW, plot.B);
    const IRECT barS(barI.R, plot.T, barI.R + kLoudBarW, plot.B);
    const IRECT barM(barS.R, plot.T, barS.R + kLoudBarW, plot.B);

    float topL, botL;
    LoudWindow(topL, botL);
    const bool tgtValid = mTarget > -100.f;

    const IText t(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Bottom);
    auto drawTickText = [&](float v, const IRECT &labelR) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "%d", (int)std::lround(v));
      g.DrawText(t, buf, labelR);
    };
    auto hiddenByChip = [&](const IRECT &labelR) {
      return !skipRect.Empty() && labelR.Intersects(skipRect);
    };
    // 刻度按钮: 无常驻底色，hover 时垫半透明遮罩
    auto drawTickBtn = [&](float v, const IRECT &labelR) {
      const IRECT btn = LoudTickBtnRect(labelR);
      if (mHoverActive && btn.Contains(mHoverX, mHoverY))
        g.FillRect(HoverOverlay(), btn);
      drawTickText(v, labelR);
    };

    // 顶刻度按钮 (窗顶值，点击循环偏移 +9/+18)
    const IRECT topLabel = LufsTickRect(plot, topL);
    if (!hiddenByChip(topLabel))
      drawTickBtn(topL, topLabel);

    // 目标刻度按钮 (点击循环预设 -9/-14/-23/-24)
    if (tgtValid) {
      const IRECT midLabel = LufsTickRect(plot, mTarget);
      if (!hiddenByChip(midLabel))
        drawTickBtn(mTarget, midLabel);
    }

    // 底刻度 (纯文字)
    const IRECT botLabel = LufsTickRect(plot, botL);
    if (!hiddenByChip(botLabel))
      drawTickText(botL, botLabel);

    // 目标线 (跨三条)
    if (tgtValid) {
      const float yT = LoudYOf(plot, mTarget);
      g.FillRect(COL_900(), IRECT(barI.L, yT - 1.f, barM.R, yT + 1.f));
    }

    // 语义色段 (目标相对: ≤目标-1 黄 / |Δ|≤1 绿 / ≥目标+1 红)
    struct LStop { float lufs; IColor c; };
    const LStop stops[] = {
      {botL, SemColor(MeterYellow())},
      {mTarget - 1.f, SemColor(MeterGreen())},
      {mTarget + 1.f, SemColor(MeterRed())},
      {topL, SemColor(MeterRed())},
    };
    const int nStops = (int)(sizeof(stops) / sizeof(stops[0]));
    const float seamOv = 1.f / std::max(1.f, g.GetScreenScale() * g.GetDrawScale());

    auto drawBar = [&](const IRECT &bar, float lufs, const char *label) {
      g.FillRect(COL_300(), bar);
      if (lufs > -99.f) {
        const float yTop = LoudYOf(plot, lufs);
        for (int i = 0; i + 1 < nStops; ++i) {
          const float yA = LoudYOf(plot, stops[i].lufs);
          const float yB = LoudYOf(plot, stops[i + 1].lufs);
          if (yA <= yTop)
            continue;
          const float rT = std::max(yB - seamOv, yTop);
          const float rB = std::min(yA + seamOv, bar.B);
          if (rB - rT <= 0.f)
            continue;
          IPattern grad = IPattern::CreateLinearGradient(bar.L, yA, bar.L, yB);
          grad.AddStop(stops[i].c, 0.f);
          grad.AddStop(stops[i + 1].c, 1.f);
          g.PathClear();
          g.PathRect(IRECT(bar.L, rT, bar.R, rB));
          g.PathFill(grad);
        }
      }
      const IText lbl(10.f, COL_500(), kFontRegular, EAlign::Center, EVAlign::Top);
      g.DrawText(lbl, label, IRECT(bar.L, plot.T, bar.R, plot.T + 12.f));
    };
    drawBar(barI, mIntegrated, "I");
    drawBar(barS, mShortTerm, "S");
    drawBar(barM, mMomentary, "M");

    // 底部水印: I 条显示当前值，S/M 条拼 "LUFS" 单位，各自随条渐变状态取色
    const float kMarkH = 19.f;
    auto loudMarkActive = [&](float lufs) { return lufs > -99.f && lufs > botL; };
    auto drawLoudMark = [&](const IRECT &bar, const char *txt, bool active) {
      IRECT box(bar.L, plot.B - kMarkH, bar.R, plot.B);
      float size = 11.f;
      IColor col = active ? COL_300() : COL_700();
      IText t(size, col, kFontSemiBold, EAlign::Center, EVAlign::Middle);
      for (;;) {
        IRECT m = box;
        g.MeasureText(t, txt, m);
        if ((m.L >= box.L - 0.5f && m.R <= box.R + 0.5f) || size <= 8.f)
          break;
        size -= 1.f;
        t = IText(size, col, kFontSemiBold, EAlign::Center, EVAlign::Middle);
      }
      g.DrawText(t, txt, box);
    };
    const bool iAct = loudMarkActive(mIntegrated);
    const bool sAct = loudMarkActive(mShortTerm);
    const bool mAct = loudMarkActive(mMomentary);
    char iBuf[16];
    if (mIntegrated > -99.f)
      std::snprintf(iBuf, sizeof(iBuf), "%.1f", mIntegrated);
    else
      std::snprintf(iBuf, sizeof(iBuf), "%s", "—");
    drawLoudMark(barI, iBuf, iAct);
    drawLoudMark(barS, "LU", sAct);
    drawLoudMark(barM, "FS", mAct);
  }

  // LRA bracket (Pro-L2 式): M 条右侧，仅 mLraValid 时绘制
  void DrawLraBracket(IGraphics &g, const IRECT &plot) {
    if (!mLraValid)
      return;
    float topL, botL;
    LoudWindow(topL, botL);
    const float yTop = LoudYOf(plot, std::clamp(mLraMax, botL, topL));
    const float yBot = LoudYOf(plot, std::clamp(mLraMin, botL, topL));
    if (yBot - yTop < 1.f)
      return;

    const float zoneL = plot.R + kMeterStripW - kLraZoneW;
    const float zoneR = plot.R + kMeterStripW;
    const IColor col = SemColor(MeterYellow());

    constexpr float kSpineW = 1.5f, kCapH = 1.5f, kCapLen = 6.f;
    g.FillRect(col, IRECT(zoneL, yTop, zoneL + kSpineW, yBot));
    g.FillRect(col, IRECT(zoneL, yTop - kCapH * 0.5f, zoneL + kCapLen, yTop + kCapH * 0.5f));
    g.FillRect(col, IRECT(zoneL, yBot - kCapH * 0.5f, zoneL + kCapLen, yBot + kCapH * 0.5f));

    char valBuf[16];
    std::snprintf(valBuf, sizeof(valBuf), "%.1f", mRange);
    const float midY = (yTop + yBot) * 0.5f;
    const float span = yBot - yTop;
    const float labelSize = std::clamp(span * 0.28f, 9.f, 12.f);
    const float valSize = std::clamp(span * 0.42f, 11.f, 16.f);
    const float gap = 1.f;
    const float labelH = labelSize + 2.f;
    const float valH = valSize + 2.f;
    const float totalH = labelH + gap + valH;
    const float topLimit = std::max(yTop + 1.f, yBot - totalH - 1.f);
    const float blockT = std::min(std::max(midY - totalH * 0.5f, yTop + 1.f), topLimit);
    const IText labelT(labelSize, col, kFontRegular, EAlign::Far, EVAlign::Middle);
    g.DrawText(labelT, "LRA", IRECT(zoneL + kCapLen + 1.f, blockT, zoneR - 1.f, blockT + labelH));
    const IText valT(valSize, col, kFontSemiBold, EAlign::Far, EVAlign::Middle);
    g.DrawText(valT, valBuf, IRECT(zoneL + kCapLen + 1.f, blockT + labelH + gap, zoneR - 1.f,
                                   blockT + labelH + gap + valH));
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
    } else {
      val = ch ? mPeakR : mPeakL;
      rmsVal = ch ? mRmsR : mRmsL;
      hold = ch ? mHoldR : mHoldL;
      over = ch ? mOverR : mOverL;
    }
    g.FillRect(COL_300(), bar);

    auto tOf = [&](float db) {
      return std::clamp((kTopDb - db) / (kTopDb - mBottomDb), 0.f, 1.f);
    };

    // 条色随主题饱和档位映射 (锚点分段线性)
    const int satClamp = std::min(ThemeSatMax(), 50);
    const float satScale = (satClamp <= 15) ? ((float)satClamp / 60.f)
                                            : (0.25f + (float)(satClamp - 15) / 140.f);

    struct MeterStop { float t; int h; float s; float b; };
    const MeterStop stops[] = {
      {0.f, 0, 0.73f, 0.87f},
      {tOf(0.f), 4, 0.71f, 0.92f},
      {tOf(-6.f), 32, 0.78f, 0.94f},
      {tOf(-14.f), 50, 0.73f, 0.86f},
      {tOf(-24.f), 140, 0.58f, 0.71f},
      {tOf(-48.f), 150, 0.68f, 0.60f},
      {1.f, 156, 0.70f, 0.48f},
    };
    const int nStops = (int)(sizeof(stops) / sizeof(stops[0]));

    auto meterColor = [&](const MeterStop &st, int alpha) {
      const float s = std::clamp(st.s * satScale, 0.f, 1.f);
      const IColor c = HSBToIColor(st.h, s, st.b);
      return IColor(alpha, c.R, c.G, c.B);
    };

    auto colorAtT = [&](float t, int alpha) {
      for (int i = 0; i + 1 < nStops; ++i) {
        if (t <= stops[i + 1].t) {
          const float span = stops[i + 1].t - stops[i].t;
          const float f = (span > 0.f) ? (t - stops[i].t) / span : 0.f;
          const IColor a = meterColor(stops[i], alpha);
          const IColor b = meterColor(stops[i + 1], alpha);
          return IColor(alpha, (int)(a.R + (b.R - a.R) * f + 0.5f),
                              (int)(a.G + (b.G - a.G) * f + 0.5f),
                              (int)(a.B + (b.B - a.B) * f + 0.5f));
        }
      }
      return meterColor(stops[nStops - 1], alpha);
    };

    auto meterColorAt = [&](float db, int alpha) { return colorAtT(tOf(db), alpha); };

    // NanoVG 后端只支持双色渐变，按 stop 分段用 2-stop 渐变，段间重叠 1 设备像素
    const float seamOv = 1.f / std::max(1.f, g.GetScreenScale() * g.GetDrawScale());
    auto fillGrad = [&](float yTop, int alpha) {
      if (yTop >= bar.B)
        return;
      for (int i = 0; i + 1 < nStops; ++i) {
        const float yA = plot.T + stops[i].t * plot.H();
        const float yB = plot.T + stops[i + 1].t * plot.H();
        if (yB <= yTop)
          continue;
        const float rT = std::max(yA - seamOv, yTop);
        const float rB = std::min(yB + seamOv, bar.B);
        if (rB - rT <= 0.f)
          continue;
        IPattern grad = IPattern::CreateLinearGradient(bar.L, yA, bar.L, yB);
        grad.AddStop(meterColor(stops[i], alpha), 0.f);
        grad.AddStop(meterColor(stops[i + 1], alpha), 1.f);
        g.PathClear();
        g.PathRect(IRECT(bar.L, rT, bar.R, rB));
        g.PathFill(grad);
      }
    };

    if (mMeterMode == 1) {
      const float yPeak = YOf(plot, val);
      const float yRms = YOf(plot, std::clamp(rmsVal, mBottomDb, kTopDb));
      fillGrad(yPeak, 90);
      fillGrad(yRms, 245);
    } else {
      fillGrad(YOf(plot, val), 255);
    }

    // 峰值保持线: dBFS 模式下条体是 90 alpha 叠加背景，按相同比例合成避免偏亮
    if (mHoldSec > 0.f && hold > mBottomDb) {
      const float yH = YOf(plot, std::clamp(hold, mBottomDb, top));
      IColor hc = meterColorAt(hold, 255);
      if (mMeterMode == 1) {
        const IColor bg = COL_300();
        const float a = 90.f / 255.f;
        hc = IColor(255, (int)(hc.R * a + bg.R * (1.f - a) + 0.5f),
                         (int)(hc.G * a + bg.G * (1.f - a) + 0.5f),
                         (int)(hc.B * a + bg.B * (1.f - a) + 0.5f));
      }
      g.FillRect(hc, IRECT(bar.L, yH - 1.f, bar.R, yH + 1.f));
    }

    // dBTP 持久锁存: 真峰值越过 0 dBFS 后锁存，只增不减，点击条重置；只在 L 条绘制一次
    if (ch == 0 && mMeterMode == 0 && mHoldSec > 0.f) {
      const float persistMax = std::max(mPersistL, mPersistR);
      if (persistMax > 0.f && persistMax > mBottomDb) {
        const float yP = YOf(plot, std::clamp(persistMax, mBottomDb, top));
        char pbuf[8];
        if (std::fabs(persistMax) >= 10.f)
          std::snprintf(pbuf, sizeof(pbuf), "%d", (int)std::lround(persistMax));
        else
          std::snprintf(pbuf, sizeof(pbuf), "%.1f", persistMax);
        const IText pt(14, meterColorAt(kTopDb, 255), kFontSemiBold, EAlign::Center, EVAlign::Middle);
        constexpr float kPersistLblH = 17.f;
        const float prT = std::max(yP - kPersistLblH * 0.5f, plot.T);
        g.DrawText(pt, pbuf, IRECT(bar.L, prT, bar.R + kGainBarW, prT + kPersistLblH));
      }
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

  // 每 20dB 刻度文字，画在频谱区内部右侧；hover 读数重叠时隐藏
  void DrawDbGrid(IGraphics &g, const IRECT &plot, const IRECT &skipRect) {
    if (mBottomDb >= 0.f)
      return;

    const int bottomDb = (int)mBottomDb;
    const IText t(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Bottom);

    for (int db = 0; db >= bottomDb; db -= 20) {
      // 最底部一条由 Range 按钮顶替，跳过文字
      if (db == bottomDb)
        continue;

      const float y = plot.B - (float)(db - bottomDb) / (kTopDb - (float)bottomDb) * plot.H();

      const IRECT labelR(plot.R - 52.f, y - kLabelH - 1.f, plot.R - kTickRight, y - 1.f);
      if (!skipRect.Empty() && labelR.Intersects(skipRect))
        continue;

      char buf[16];
      std::snprintf(buf, sizeof(buf), "%d", db);
      g.DrawText(t, buf, labelR);
    }
  }

  // 频率刻度: 20Hz/100Hz/1kHz/10kHz，位于频谱内部顶端；hover 读数重叠时隐藏
  void DrawFreqGrid(IGraphics &g, const IRECT &plot, const IRECT &skipRect) {
    struct FreqLabel {
      double hz;
      const char *txt;
    };
    static const FreqLabel kLabels[] = {{20., "20Hz"}, {100., "100Hz"}, {1000., "1kHz"}, {10000., "10kHz"}};
    const IText t(14, COL_700(), kFontRegular, EAlign::Near, EVAlign::Top);
    for (const auto &lbl : kLabels) {
      const float x = XOf(plot, lbl.hz);
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

  // ── 显示域弹道 ──
  // 弹道在显示域运行: 目标 = 原始幅度 dB + 斜率，钳到显示范围。
  // 由此屏上运动轨迹只由弹道参数决定，与 Range/斜率/信号电平无关，回落无吊尾。

  // 弹道目标: 原始 dB + 斜率，底部带 ~10% 屏高缓冲 (Pro-Q 式，避免渐近屏底拖尾)
  float SmoothTarget(float rawDb, float tiltDb) const {
    const float over = 0.1f * (kTopDb - mBottomDb);
    return std::clamp(rawDb + tiltDb, mBottomDb - over, kTopDb);
  }

  // 单点弹道: 攻击 dB 域单极点; 回落 LOG=等比收缩, UNIF=恒定屏高比例速率
  float StepSmoothed(float prevDb, float targetDb, float aCoef, float rCoef, float unifStepDb) const {
    if (targetDb > prevDb)
      return aCoef * prevDb + (1.f - aCoef) * targetDb;
    if (mReleaseMode == 1)
      return std::max(targetDb, prevDb - unifStepDb);
    return rCoef * prevDb + (1.f - rCoef) * targetDb;
  }

  // 合并显示 dB: PWR = 功率和, SUM = 幅度和
  static float MergeDb(float dL, float dR, int algo) {
    if (algo == 0) {
      const float p = std::exp2f(dL * 0.33219280949f) + std::exp2f(dR * 0.33219280949f);
      return 3.01029995664f * orm::FastLog2(p);
    }
    const float a = std::exp2f(dL * 0.16609640474f) + std::exp2f(dR * 0.16609640474f);
    return 6.02059991328f * orm::FastLog2(a);
  }

  // FFT 模式: bin -> 256 band 聚合 (取 max) 后做显示域弹道；空桶不参与绘制
  void ProcessFFTBands(ISenderData<3, TDataPacket> &d, float a, float r, float unifStepDb) {
    if ((int)mBinToBand.size() != mNumBins)
      RebuildBinToBand();
    const int nb = std::min(mNumBins, (int)d.vals[0].size());
    if (mSpectrum[0].size() != (size_t)kSpectrumBands)
      for (int c = 0; c < 3; ++c)
        mSpectrum[c].assign(kSpectrumBands, -150.f);

    std::array<std::array<float, kSpectrumBands>, 3> bandMax;
    std::array<float, 3> anchor{};
    for (int c = 0; c < 3; ++c)
      bandMax[c].fill(0.f);
    mBandUsed.fill(false);
    bool anchorUsed = false;

    for (int i = 0; i < nb; ++i) {
      const int b = mBinToBand[i];
      if (b == kSubBand) { // 20Hz 下方锚点 (轴外，只用于入场线斜率)
        for (int c = 0; c < 3; ++c)
          if (d.vals[c][i] > anchor[c])
            anchor[c] = d.vals[c][i];
        anchorUsed = true;
        continue;
      }
      if (b < 0)
        continue;
      mBandUsed[b] = true;
      for (int c = 0; c < 3; ++c)
        if (d.vals[c][i] > bandMax[c][b])
          bandMax[c][b] = d.vals[c][i];
    }

    // 空桶不造值，由贝塞尔曲线在真实 band 点间插值；首带之前用锚点外推入场线
    const double logLo = std::log2(kSpecFreqLo);
    const double logBand = (std::log2(kSpecFreqHi) - logLo) / kSpectrumBands;
    int first = 0;
    while (first < kSpectrumBands && !mBandUsed[first])
      ++first;
    if (first < kSpectrumBands) {
      const double fFirst = kSpecFreqLo * std::exp2(logBand * (first + 0.5));
      for (int c = 0; c < 3; ++c) {
        const float aFirst = bandMax[c][first];
        const float aA = anchor[c];
        const bool haveAnchor = anchorUsed && aA > 1e-9f && aFirst > 1e-9f;
        for (int b = 0; b < first; ++b) {
          float ext = aFirst;
          if (haveAnchor) {
            const double fb = kSpecFreqLo * std::exp2(logBand * (b + 0.5));
            const double t = std::log2(fb / (double)kSpecAnchorHz) /
                             std::log2(fFirst / (double)kSpecAnchorHz);
            ext = aA * std::pow(aFirst / aA, (float)t);
          }
          bandMax[c][b] = ext;
        }
      }
      // 入场段只画左缘一点，整段共线会在首带形成折角
      mBandUsed[0] = true;
    }

    const bool hasTilt = !mSlopeDb.empty();
    for (int c = 0; c < 3; ++c)
      for (int b = 0; b < kSpectrumBands; ++b) {
        const float rawDb =
            (bandMax[c][b] > 1e-30f) ? 6.02059991328f * orm::FastLog2(bandMax[c][b]) : -150.f;
        const float target = SmoothTarget(rawDb, hasTilt ? mSlopeDb[b] : 0.f);
        mSpectrum[c][b] = StepSmoothed(mSpectrum[c][b], target, a, r, unifStepDb);
      }
  }

  // 频谱峰值保持: 开关/时长与电平表 hold 共用；超时后按 20 dB/s 回落
  void UpdatePeakHold() {
    const int nVals = (int)mSpectrum[0].size();
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

    // ∞ 档 (1e9) 恒不回落
    const float fallDb = (mHoldSec < 1e8f && dt > 0.f) ? 20.f * dt : 0.f;
    for (int c = 0; c < 3; ++c) {
      if (mHoldSpec[c].size() != (size_t)nVals) {
        mHoldSpec[c].assign(nVals, -1000.f);
        mHoldAge[c].assign(nVals, 0.f);
        mHoldSignal = false;
      }
      for (int i = 0; i < nVals; ++i) {
        const float cur = mSpectrum[c][i];
        float &hold = mHoldSpec[c][i];
        if (cur > hold) {
          hold = cur;
          mHoldAge[c][i] = 0.f;
          if (cur > mBottomDb + 0.5f)
            mHoldSignal = true;
        } else if (fallDb > 0.f) {
          mHoldAge[c][i] += dt;
          if (mHoldAge[c][i] > mHoldSec)
            hold = std::max(cur, hold - fallDb);
        }
      }
    }
  }

  // hold 曲线单点值: LR 取双通道较大者, MERGE 用与显示曲线相同的 merge 公式
  float HoldValAt(int b) const {
    const float hL = (mHoldSpec[0].size() > (size_t)b) ? mHoldSpec[0][b] : -1000.f;
    const float hR = (mHoldSpec[1].size() > (size_t)b) ? mHoldSpec[1][b] : -1000.f;
    if (mChanMode == 0)
      return (hL > hR) ? hL : hR;
    const float hSum = (mHoldSpec[2].size() > (size_t)b) ? mHoldSpec[2][b] : -1000.f;
    return (mMergeAlgo == 0) ? MergeDb(hL, hR, 0) : hSum;
  }

  void DrawSpectrum(IGraphics &g, const IRECT &plot) {
    if (mSpectrum[0].empty() || mSpectrum[1].empty())
      return;
    if (plot.W() <= 0.f || plot.H() <= 0.f)
      return;

    IColor cL, cR, cO;
    GetChannelColors(cL, cR, cO);

    const float span = kTopDb - mBottomDb;
    auto dbToY = [&](float db) -> float {
      return plot.B - (db - mBottomDb) / span * plot.H();
    };

    const int nb = (mMode == 1) ? (int)mVQTFreqs.size()
                 : (mMode == 2) ? (int)mPBTFreqs.size()
                 : (mMode == 3) ? (int)mRTAFreqs.size()
                                : kSpectrumBands;
    const bool smooth = (mMode != 2);

    mSpecPtsL.clear();
    mSpecPtsR.clear();
    mSpecPtsM.clear();
    mHoldPts.clear();
    const int have = std::min(nb, (int)mSpectrum[0].size());
    for (int b = 0; b < have; ++b) {
      if (mMode == 0 && !mBandUsed[b])
        continue;
      float x;
      if (mMode == 1)
        x = plot.L + mVQTFreqNorm[b] * plot.W();
      else if (mMode == 2)
        x = plot.L + mPBTFreqNorm[b] * plot.W();
      else if (mMode == 3)
        x = plot.L + mRTAFreqNorm[b] * plot.W();
      else
        x = plot.L + mBandNormX[b] * plot.W();

      const float dL = mSpectrum[0][b];
      const float dR = mSpectrum[1][b];
      const float dSum = (mSpectrum[2].size() > (size_t)b) ? mSpectrum[2][b] : mBottomDb;
      mSpecPtsL.push_back({x, dbToY(dL)});
      mSpecPtsR.push_back({x, dbToY(dR)});

      const float dM = (mMergeAlgo == 0) ? MergeDb(dL, dR, 0) : dSum;
      mSpecPtsM.push_back({x, dbToY(dM)});
      mHoldPts.push_back({x, dbToY(HoldValAt(b))});
    }

    // 整条曲线都低于底缘时跳过，避免底缘残留抗锯齿余迹
    auto allBelow = [&](const std::vector<Pt> &pts) {
      for (const Pt &p : pts)
        if (p.y < plot.B)
          return false;
      return true;
    };

    if (mChanMode == 0) {
      if (!allBelow(mSpecPtsL))
        DrawFill(g, plot, mSpecPtsL, cL, kGradientMinAlpha, kLayerTopAlpha, smooth, false);
      if (!allBelow(mSpecPtsR))
        DrawFill(g, plot, mSpecPtsR, cR, kGradientMinAlpha, kLayerTopAlpha, smooth, true);
    } else {
      if (!allBelow(mSpecPtsM))
        DrawFill(g, plot, mSpecPtsM, cO, kGradientMinAlpha, 255, smooth);
    }
    if (!allBelow(mHoldPts))
      DrawHoldCurve(g, plot, smooth);
  }

  // 建开放曲线主路径 (右缘吸附 + 平滑/折线)，供填充与 hold 细线共用
  void BuildCurvePath(IGraphics &g, const IRECT &plot, std::vector<Pt> &pts, bool smooth) {
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
    g.PathLineTo(pts.back().x, plot.B);
    g.PathLineTo(plot.L, plot.B);
    g.PathClose();

    // 渐变范围跟随曲线峰值，保证弱信号在底部也有对比度
    float topY = plot.B;
    for (const Pt &p : pts)
      topY = std::min(topY, p.y);
    const IRECT gradRect(plot.L, topY, plot.R, plot.B);
    // 指数衰减渐变: alpha 权重首帧预计算，每帧只做乘加
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

  // 频谱峰值保持细线: 半透明，平滑策略与显示曲线一致
  void DrawHoldCurve(IGraphics &g, const IRECT &plot, bool smooth) {
    if (mHoldSec <= 0.f || !mHoldSignal || mHoldPts.size() < 2)
      return;
    BuildCurvePath(g, plot, mHoldPts, smooth);
    const IColor c(kHoldLineAlpha, COL_900().R, COL_900().G, COL_900().B);
    g.PathStroke(IPattern(c), 1.f);
  }

  static constexpr int kGradientMinAlpha = 15;
  static constexpr int kLayerTopAlpha = 200;
  static constexpr int kHoldLineAlpha = 75;
  static constexpr int kGradientStops = 12;
  static constexpr float kGradientDecay = 3.5f;
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
  // 20Hz 下方虚拟锚点: 聚合 [10,20)Hz 的 bin，给低频空桶提供自然入场斜率
  static constexpr float kSpecAnchorLo = 10.f;
  static constexpr float kSpecAnchorHz = 14.14214f; // sqrt(10·20)
  static constexpr int kSubBand = -2;               // mBinToBand 哨兵: 归入锚点桶
  static constexpr float kTopDb = 9.f;
  static constexpr float kVuTopDb = 3.f;
  static constexpr float kVuBottomVU = -20.f;
  static constexpr float kTickRight = 3.f;
  static constexpr float kLabelH = 16.f;

  // 预计算 bin -> 对数 band 映射表，只在采样率/FFT 尺寸变化时重建
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
          mBinToBand[i] = kSubBand;
        continue;
      }
      const int b = (int)((std::log2(f) - logLo) / logBand);
      if (b >= 0 && b < kSpectrumBands)
        mBinToBand[i] = b;
    }
  }

  std::vector<float> mSpectrum[3]; // 平滑后的显示 dB (已含斜率)
  float mPeakL = -120.f, mPeakR = -120.f;
  float mTrueL = -120.f, mTrueR = -120.f;
  float mRmsL = -120.f, mRmsR = -120.f;
  float mVuL = -120.f, mVuR = -120.f;
  float mVuHoldL = -120.f, mVuHoldR = -120.f;
  float mPersistL = -120.f, mPersistR = -120.f;
  float mHoldL = -1000.f, mHoldR = -1000.f;
  float mHoldSec = 2.f;
  int mMeterMode = 0; // 0=dBTP, 1=dBFS+RMS
  bool mOverL = false, mOverR = false;
  float mMomentary = -120.f, mShortTerm = -120.f;
  float mIntegrated = -120.f;
  float mRange = 0.f;
  float mLraMin = -120.f, mLraMax = -120.f;
  bool mLraValid = false;
  float mTarget = -14.f;
  float mScaleOff = 9.f;
  std::vector<int> mBinToBand;
  std::array<bool, kSpectrumBands> mBandUsed{};
  int mMode = 0; // 0=FFT, 1=VQT, 2=PBT, 3=RTA
  int mChanMode = 0; // 0=L/R, 1=MERGE
  int mMergeAlgo = 0; // 0=PWR, 1=SUM
  std::vector<float> mVQTFreqs;
  std::vector<float> mVQTFreqNorm;
  std::vector<float> mPBTFreqs;
  std::vector<float> mPBTFreqNorm;
  std::vector<float> mRTAFreqs;
  std::vector<float> mRTAFreqNorm;
  std::array<float, kSpectrumBands> mBandNormX{};

  // 频谱斜率: tiltDb(f) = S·log2(f/f_pivot)，在弹道前施加
  static constexpr float kSlopeRefHz = 632.45553f; // 支点 = sqrt(20·20000)
  float mSlopeDbPerOct = 0.f;
  std::vector<float> mSlopeDb;

  void RebuildSlopeGain() {
    std::vector<float> *freqs = nullptr;
    int n = 0;
    if (mMode == 0) {
      n = kSpectrumBands;
    } else {
      freqs = (mMode == 1) ? &mVQTFreqs : (mMode == 2) ? &mPBTFreqs : &mRTAFreqs;
      n = (int)freqs->size();
    }
    mSlopeDb.assign(n, 0.f);
    if (mSlopeDbPerOct == 0.f || n <= 0)
      return;
    const double logLo = std::log2(kSpecFreqLo);
    const double logBand = (std::log2(kSpecFreqHi) - logLo) / kSpectrumBands;
    for (int b = 0; b < n; ++b) {
      const float f = (mMode == 0) ? (float)(kSpecFreqLo * std::exp2(logBand * (b + 0.5)))
                                   : (*freqs)[b];
      mSlopeDb[b] = mSlopeDbPerOct * orm::FastLog2(f / kSlopeRefHz);
    }
  }
  float mAttackCoeff = 0.2f;
  float mReleaseCoeff = 0.9f;
  int mReleaseMode = 0; // 0=LOG, 1=匀速
  float mAttackSec = 0.05f;
  float mReleaseSec = 0.2f;
  float mBottomDb = -100.f;
  int mNumBins = 2048;
  double mSampleRate = 48000.0;

  std::vector<Pt> mSpecPtsL;
  std::vector<Pt> mSpecPtsR;
  std::vector<Pt> mSpecPtsM;
  std::vector<Pt> mHoldPts;

  // 频谱峰值保持 (与 mSpectrum 同域同长)
  std::vector<float> mHoldSpec[3];
  std::vector<float> mHoldAge[3];
  bool mHoldSignal = false;
  bool mHoldTpValid = false;
  bool mHoldWasActive = false;
  std::chrono::steady_clock::time_point mLastHoldTp{};

  bool mHoverActive = false;
  float mHoverX = 0.f;
  float mHoverY = 0.f;

  // 静态网格离屏缓存
  ILayerPtr mGridLayer;
  float mGridBottomDb = -1000.f;
  int mGridHue = -1;
  int mGridSat = -1;
  int mGridMode = -1;

  // 绘制耗时测量 (快捷键 P 开关 HUD)
  static constexpr int kHudN = 120;
  float mHudMs[kHudN] = {};
  uint8_t mHudIsData[kHudN] = {};
  double mHudT[kHudN] = {};
  int mHudPos = 0;
  int mHudFill = 0;
  uint32_t mDataPkts = 0;
  bool mHudOn = false;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
