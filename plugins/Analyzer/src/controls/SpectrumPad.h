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
  // 尺寸必须与四个分析引擎的数据包严格一致 (ISender 整体拷贝)
  using TDataPacket = std::array<float, 8192>;

  enum MsgTags {
    kMsgTagSampleRate = 1,
    kMsgTagFFTSize,
    kMsgTagRelease,
    kMsgTagReleaseMode,
    kMsgTagRange,
    kMsgTagSlope,
    kMsgTagMode,
    kMsgTagVQTBands,
    kMsgTagReset,
    kMsgTagChanMode,
    kMsgTagMergeAlgo,
    kMsgTagLevelMeter,
    kMsgTagLoudness,
    kMsgTagPBTBands,
    kMsgTagRTABands,
  };

  enum EMeterClick {
    kClickResetPersist,
    kClickResetMeterHold,
    kClickResetOver,
    kClickLoudScale,
    kClickLoudPreset,
  };
  std::function<void(EMeterClick)> mMeterClickHandler;

  SpectrumPad(const IRECT &bounds) : IControl(bounds) {
    mSpecPtsL.reserve(kSpectrumBands);
    mSpecPtsR.reserve(kSpectrumBands);
    mSpecPtsM.reserve(kSpectrumBands);
    RebuildBinToBand();
    const double logLo = std::log2(kSpecFreqLo);
    const double logHi = std::log2(kSpecFreqHi);
    const double logBand = (logHi - logLo) / kSpectrumBands;
    for (int b = 0; b < kSpectrumBands; ++b) {
      const double fCenter = kSpecFreqLo * std::exp2(logBand * (b + 0.5));
      mBandNormX[b] = FreqNorm(fCenter);
    }
    mHoldPts.reserve(kSpectrumBands);
  }

  void ClearPeakHold() {
    for (int c = 0; c < 3; ++c) {
      mHoldSpec[c].assign(mHoldSpec[c].size(), -1000.f);
      mHoldAge[c].assign(mHoldAge[c].size(), 0.f);
    }
    mHoldSignal = false;
    SetDirty(false);
  }

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
      const int nVals = (mMode == 0) ? std::max(mNumBins, 0)
                                     : (mMode == 1) ? (int)mVQTFreqs.size()
                                     : (mMode == 2) ? (int)mPBTFreqs.size()
                                                    : (int)mRTAFreqs.size();
      if (nVals <= 0)
        return;

      const double hop = 1024.0;
      const double updatePeriod = hop / std::max(mSampleRate, 1.0);
      mAttackCoeff = (float)std::exp(-updatePeriod / mAttackSec);
      mReleaseCoeff = (float)std::exp(-updatePeriod / mReleaseSec);
      const float unifStepDb = (float)(updatePeriod / (2.0 * std::max(mReleaseSec, 1e-3f)) * (kTopDb - mBottomDb));

      const float a = mAttackCoeff, r = mReleaseCoeff;
      if (mMode == 0) {
        ProcessFFTBands(d, a, r, unifStepDb);
      } else {
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
              // PBT 起振 τ = 1/(π·bw): 窄带低频自然蓄力爬坡
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
      RebuildSlopeGain();
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
    const IRECT plot = mRECT.GetReducedFromRight(kMeterStripW)
                           .GetPixelAligned(g.GetScreenScale() * g.GetDrawScale());

    const float zoneLr = LrZoneR(plot);
    const float zoneVu = VuZoneR(plot);
    const float zoneLra = plot.R + kMeterStripW - kLraZoneW;
    const bool ledArea = (mMeterMode == 1) && (mHoverX > plot.R) &&
                         (mHoverX <= zoneLr) && (mHoverY < YOf(plot, 0.f) - 3.f);
    const bool inStrip = mHoverActive && !ledArea && mHoverX >= plot.L && mHoverX <= mRECT.R &&
                         mHoverY >= plot.T && mHoverY <= plot.B;
    const bool inSpectrum = inStrip && mHoverX <= plot.R;
    const bool inZone0 = inStrip && mHoverX <= zoneLr;
    // 刻度文字列 (VU/LUFS) 与 LRA 括弧列不参与 hover, 悬停线只覆盖各自的量条
    const bool inZone1 = inStrip && mHoverX > zoneLr + kVuScaleW && mHoverX <= zoneVu;
    const bool inZone2 = inStrip && mHoverX > zoneVu + kLufsScaleW && mHoverX <= zoneLra;
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
        vuText = IText(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Middle);
        const float scaleVu = LrZoneR(plot);
        float vuT = yLine - kLabelH * 0.5f; // 读数优先上下居中于悬停线, 贴缘时收敛进画区
        if (vuT + kLabelH > plot.B)
          vuT = plot.B - kLabelH;
        if (vuT < plot.T)
          vuT = plot.T;
        vuBox = IRECT(scaleVu, vuT, scaleVu + kVuScaleW - kTickRight, vuT + kLabelH);
        g.MeasureText(vuText, vuBuf, vuBox);
        vuChip = vuBox.GetPadded(2.f);
        vuSkip = vuChip;
      } else {
        float topL, botL;
        LoudWindow(topL, botL);
        const float lufs = botL + (plot.B - yLine) / plot.H() * (topL - botL);
        std::snprintf(lufsBuf, sizeof(lufsBuf), "%+.1f", lufs);
        lufsText = IText(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Middle);
        const float scaleLufs = VuZoneR(plot);
        float lufsT = yLine - kLabelH * 0.5f; // 同 VU: 居中于悬停线, 贴缘时收敛进画区;
        if (lufsT + kLabelH > plot.B)         // 与刻度按钮重叠时直接盖在其上, 不做避让
          lufsT = plot.B - kLabelH;
        if (lufsT < plot.T)
          lufsT = plot.T;
        lufsBox = IRECT(scaleLufs, lufsT, scaleLufs + kLufsScaleW - kTickRight, lufsT + kLabelH);
        g.MeasureText(lufsText, lufsBuf, lufsBox);
        lufsChip = lufsBox.GetPadded(2.f);
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
      g.DrawLine(COL_700(), zoneLr + kVuScaleW, yLine, zoneVu, yLine, nullptr, 1.f);
      g.DrawText(vuText, vuBuf, vuBox);
    } else if (inZone2) {
      g.DrawLine(COL_700(), zoneVu + kLufsScaleW, yLine, zoneLra, yLine, nullptr, 1.f);
      g.DrawText(lufsText, lufsBuf, lufsBox);
    }
  }

private:
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

  void DrawGridLayer(IGraphics &g, const IRECT &plot) {
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

  static float FreqNorm(double hz) {
    return (float)(std::log(std::clamp(hz, 20.0, 20000.0) / 20.0) / std::log(20000.0 / 20.0));
  }

  static void FreqToNoteName(double hz, char *out, int outSize) {
    static const char *const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int nn = (int)std::lround(12.0 * std::log2(hz / 440.0)) + 69;
    std::snprintf(out, outSize, "%s%d", kNoteNames[(nn % 12 + 12) % 12], nn / 12 - 1);
  }

  float XOf(const IRECT &plot, double f) const { return plot.L + FreqNorm(f) * plot.W(); }

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

  float MeterTopDb() const {
    return (mMeterMode == 1) ? 0.f : kTopDb;
  }

  float YOf(const IRECT &plot, float db) const {
    return plot.B - (db - mBottomDb) / (kTopDb - mBottomDb) * plot.H();
  }

  float LrZoneR(const IRECT &plot) const { return plot.R + 2.f * kGainBarW; }
  float VuZoneR(const IRECT &plot) const { return LrZoneR(plot) + kVuScaleW + 2.f * kVuBarW; }

  IRECT VuTickRect(const IRECT &plot, float vu) const {
    const float scaleL = LrZoneR(plot);
    if (vu >= kVuTopDb)
      return IRECT(scaleL, plot.T + 1.f, scaleL + kVuScaleW - kTickRight, plot.T + 1.f + kLabelH);
    const float y = plot.B - (vu - kVuBottomVU) / (kVuTopDb - kVuBottomVU) * plot.H();
    return IRECT(scaleL, y - kLabelH - 1.f, scaleL + kVuScaleW - kTickRight, y - 1.f);
  }

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

  void DrawVuBar(IGraphics &g, const IRECT &plot) {
    const float scaleL = plot.R + 2.f * kGainBarW;
    const IRECT barL(scaleL + kVuScaleW, plot.T, scaleL + kVuScaleW + kVuBarW, plot.B);
    const IRECT barR(barL.R, plot.T, barL.R + kVuBarW, plot.B);

    auto tOfV = [](float vu) {
      return std::clamp((kVuTopDb - vu) / (kVuTopDb - kVuBottomVU), 0.f, 1.f);
    };
    auto yOfV = [&](float vu, const IRECT &bar) {
      return bar.B - (vu - kVuBottomVU) / (kVuTopDb - kVuBottomVU) * bar.H();
    };

    // OKLCH 色标：-3 以下恒绿，-3~+3 (表头顶段) 绿→黄→红逐档过渡。
    // 绿→红跨度大，单段 sRGB 插值会拉出一条橄榄色，故拆成多段窄渐变、每段色相相近
    struct VuStop {
      float t;
      float l, c, h;
    };
    const float vuSeam = tOfV(-3.f);
    const VuStop kVuStopsLight[] = {
      {0.f,             0.620f, 0.205f, 27.f},
      {vuSeam * 0.33f,  0.600f, 0.185f, 65.f},
      {vuSeam * 0.67f,  0.578f, 0.158f, 102.f},
      {vuSeam,          0.560f, 0.135f, 140.f},
      {1.f,             0.420f, 0.095f, 165.f},
    };
    const VuStop kVuStopsDark[] = {
      {0.f,             0.720f, 0.200f, 25.f},
      {vuSeam * 0.33f,  0.700f, 0.180f, 63.f},
      {vuSeam * 0.67f,  0.678f, 0.158f, 100.f},
      {vuSeam,          0.660f, 0.145f, 138.f},
      {1.f,             0.510f, 0.085f, 165.f},
    };
    const VuStop *stops = ThemeMode() ? kVuStopsDark : kVuStopsLight;
    const int nStops = (int)(sizeof(kVuStopsLight) / sizeof(kVuStopsLight[0]));
    const float cScale = MeterChromaScale();

    auto meterColor = [&](const VuStop &st, int alpha) {
      const IColor c = OklchToIColor(st.l, st.c * cScale, st.h);
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

      // NanoVG 只支持 2-stop 渐变，按 stop 分段填充，段间重叠 1 设备像素
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

    const float vuMaxDb = (mVuL > mVuR) ? mVuL : mVuR;
    const IColor vuLblCol = (vuMaxDb + 18.f > kVuBottomVU) ? COL_300() : COL_700();
    const IText vuLbl(11.f, vuLblCol, kFontSemiBold, EAlign::Center, EVAlign::Middle);
    g.DrawText(vuLbl, "VU", IRECT(barL.L, plot.B - 19.f, barR.R, plot.B));
  }

  // LUFS 色标：按窗口分数布点（0=botL、1=topL，目标线恒在 2/3），所以 scaleOff 缩放时形状不变。
  // 底部绿→顶部黄、无红；绿→黄之间插一段更亮的青柠，段内 sRGB 插值才不会落进橄榄色。
  // 浅色主题顶端 L 压在 0.67：再亮就与近白的 COL_300 槽底失去明度差，即"发浅"的老问题
  struct LoudStop {
    float f;
    float l, c, h;
  };
  static const LoudStop *LoudStopTable(int &n) {
    static const LoudStop kLight[] = {
      {0.00f, 0.440f, 0.100f, 158.f}, {0.37f, 0.520f, 0.128f, 142.f},
      {0.63f, 0.560f, 0.140f, 133.f}, {0.70f, 0.610f, 0.152f, 112.f},
      {0.78f, 0.645f, 0.160f, 98.f},  {1.00f, 0.670f, 0.165f, 88.f},
    };
    static const LoudStop kDark[] = {
      {0.00f, 0.530f, 0.095f, 158.f}, {0.37f, 0.585f, 0.120f, 142.f},
      {0.63f, 0.630f, 0.135f, 133.f}, {0.70f, 0.700f, 0.150f, 112.f},
      {0.78f, 0.760f, 0.158f, 98.f},  {1.00f, 0.840f, 0.165f, 88.f},
    };
    n = (int)(sizeof(kLight) / sizeof(kLight[0]));
    return ThemeMode() ? kDark : kLight;
  }
  static IColor LoudStopColor(const LoudStop &st) {
    return OklchToIColor(st.l, st.c * MeterChromaScale(), st.h);
  }

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
    auto drawTickBtn = [&](float v, const IRECT &labelR) {
      const IRECT btn = LoudTickBtnRect(labelR);
      if (mHoverActive && btn.Contains(mHoverX, mHoverY))
        g.FillRect(HoverOverlay(), btn);
      drawTickText(v, labelR);
    };

    const IRECT topLabel = LufsTickRect(plot, topL);
    if (!hiddenByChip(topLabel))
      drawTickBtn(topL, topLabel);

    if (tgtValid) {
      const IRECT midLabel = LufsTickRect(plot, mTarget);
      if (!hiddenByChip(midLabel))
        drawTickBtn(mTarget, midLabel);
    }

    const IRECT botLabel = LufsTickRect(plot, botL);
    if (!hiddenByChip(botLabel))
      drawTickText(botL, botLabel);

    if (tgtValid) {
      const float yT = LoudYOf(plot, mTarget);
      g.FillRect(COL_900(), IRECT(barI.L, yT - 1.f, barM.R, yT + 1.f));
    }

    int nStops = 0;
    const LoudStop *stops = LoudStopTable(nStops);
    const float seamOv = 1.f / std::max(1.f, g.GetScreenScale() * g.GetDrawScale());
    auto yOfF = [&](float f) { return plot.B - std::clamp(f, 0.f, 1.f) * plot.H(); };

    auto drawBar = [&](const IRECT &bar, float lufs, const char *label) {
      g.FillRect(COL_300(), bar);
      if (lufs > -99.f) {
        const float yTop = LoudYOf(plot, lufs);
        for (int i = 0; i + 1 < nStops; ++i) {
          const float yA = yOfF(stops[i].f);
          const float yB = yOfF(stops[i + 1].f);
          if (yA <= yTop)
            continue;
          const float rT = std::max(yB - seamOv, yTop);
          const float rB = std::min(yA + seamOv, bar.B);
          if (rB - rT <= 0.f)
            continue;
          IPattern grad = IPattern::CreateLinearGradient(bar.L, yA, bar.L, yB);
          grad.AddStop(LoudStopColor(stops[i]), 0.f);
          grad.AddStop(LoudStopColor(stops[i + 1]), 1.f);
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

  void DrawLraBracket(IGraphics &g, const IRECT &plot) {
    if (!mLraValid)
      return;
    float topL, botL;
    LoudWindow(topL, botL);
    const float yTop = LoudYOf(plot, std::clamp(mLraMax, botL, topL));
    const float yBot = LoudYOf(plot, std::clamp(mLraMin, botL, topL));
    if (yBot - yTop < 1.f)
      return;
    int nStops = 0;
    const IColor col = LoudStopColor(LoudStopTable(nStops)[nStops - 1]);

    // 整体紧贴 LUFS 电平条右缘 (不再锚定列右缘); 读数竖排 (逆时针 90°, 字头朝左、自下而上读),
    // 固定最大字号、沿跨度居中; 右括号脊线贴文字右侧, 横帽向左拉到与文字左缘齐平
    constexpr float kGapBars = 4.f, kSpineW = 1.5f, kCapH = 1.5f, kTextPad = 2.f;
    const float barsR = VuZoneR(plot) + kLufsScaleW + 4.f * kLoudBarW;
    const float span = yBot - yTop;
    const float midY = (yTop + yBot) * 0.5f;
    char valBuf[16];
    std::snprintf(valBuf, sizeof(valBuf), "LRA %.1f", mRange);
    const IText txtT = IText(14.f, col, kFontSemiBold, EAlign::Center, EVAlign::Middle).WithAngle(90.f);
    IRECT mr(0.f, 0.f, 1.f, 1.f);
    g.MeasureText(txtT, valBuf, mr);

    const float textL = barsR + kGapBars;
    g.DrawText(txtT, valBuf, IRECT(textL, midY - mr.H() * 0.5f, textL + mr.W(), midY + mr.H() * 0.5f));

    // 括弧跨度容不下文字时只显示文字, 避免横帽与文字打架
    if (mr.H() > span)
      return;
    const float spineL = textL + mr.W() + kTextPad;
    g.FillRect(col, IRECT(spineL, yTop, spineL + kSpineW, yBot));
    g.FillRect(col, IRECT(textL, yTop - kCapH * 0.5f, spineL + kSpineW, yTop + kCapH * 0.5f));
    g.FillRect(col, IRECT(textL, yBot - kCapH * 0.5f, spineL + kSpineW, yBot + kCapH * 0.5f));
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

    // OKLCH 色标：每段 L 钉死 → 各段对槽底 COL_300 的明度差恒定，主题 sat 只缩放 chroma，
    // 所以低饱和主题退成对比充足的灰阶而不是发白。深色主题整体提亮，色相仍走固定语义色
    struct MeterStop { float t; float l, c, h; };
    const MeterStop kStopsLight[] = {
      {0.f,         0.560f, 0.200f, 25.f},
      {tOf(0.f),    0.600f, 0.200f, 40.f},
      {tOf(-6.f),   0.700f, 0.170f, 70.f},
      {tOf(-14.f),  0.725f, 0.145f, 88.f},
      {tOf(-24.f),  0.640f, 0.140f, 133.f},
      {tOf(-48.f),  0.500f, 0.115f, 158.f},
      {1.f,         0.410f, 0.085f, 168.f},
    };
    const MeterStop kStopsDark[] = {
      {0.f,         0.680f, 0.205f, 25.f},
      {tOf(0.f),    0.715f, 0.200f, 40.f},
      {tOf(-6.f),   0.790f, 0.170f, 70.f},
      {tOf(-14.f),  0.830f, 0.148f, 88.f},
      {tOf(-24.f),  0.740f, 0.140f, 130.f},
      {tOf(-48.f),  0.620f, 0.115f, 155.f},
      {1.f,         0.510f, 0.085f, 165.f},
    };
    const MeterStop *stops = ThemeMode() ? kStopsDark : kStopsLight;
    const int nStops = (int)(sizeof(kStopsLight) / sizeof(kStopsLight[0]));
    const float cScale = MeterChromaScale();

    auto meterColor = [&](const MeterStop &st, int alpha) {
      const IColor c = OklchToIColor(st.l, st.c * cScale, st.h);
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

    // NanoVG 只支持 2-stop 渐变，按 stop 分段填充，段间重叠 1 设备像素
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

    // dBTP 持久锁存只在 L 条绘制一次
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

  void DrawDbGrid(IGraphics &g, const IRECT &plot, const IRECT &skipRect) {
    if (mBottomDb >= 0.f)
      return;

    const int bottomDb = (int)mBottomDb;
    const IText t(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Bottom);

    for (int db = 0; db >= bottomDb; db -= 20) {
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

  float SmoothTarget(float rawDb, float tiltDb) const {
    const float over = 0.1f * (kTopDb - mBottomDb);
    return std::clamp(rawDb + tiltDb, mBottomDb - over, kTopDb);
  }

  float StepSmoothed(float prevDb, float targetDb, float aCoef, float rCoef, float unifStepDb) const {
    if (targetDb > prevDb)
      return aCoef * prevDb + (1.f - aCoef) * targetDb;
    if (mReleaseMode == 1)
      return std::max(targetDb, prevDb - unifStepDb);
    return rCoef * prevDb + (1.f - rCoef) * targetDb;
  }

  static float MergeDb(float dL, float dR, int algo) {
    if (algo == 0) {
      const float p = std::exp2f(dL * 0.33219280949f) + std::exp2f(dR * 0.33219280949f);
      return 3.01029995664f * orm::FastLog2(p);
    }
    const float a = std::exp2f(dL * 0.16609640474f) + std::exp2f(dR * 0.16609640474f);
    return 6.02059991328f * orm::FastLog2(a);
  }

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
      if (b == kSubBand) { // 20Hz 下方锚点
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
    bool edgePadded = false;
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
      const float dM = (mMergeAlgo == 0) ? MergeDb(dL, dR, 0) : dSum;
      const float yL = dbToY(dL), yR = dbToY(dR), yM = dbToY(dM), yH = dbToY(HoldValAt(b));

      // 带状首点落在 20Hz 轴内侧 (PBT L/M 档、RTA 等) 时, 先向左缘补一个同值识别点,
      // 避免填充与曲线从首点直接斜连到左下角
      if (!edgePadded && mMode != 0 && x > plot.L + 0.5f) {
        edgePadded = true;
        mSpecPtsL.push_back({plot.L, yL});
        mSpecPtsR.push_back({plot.L, yR});
        mSpecPtsM.push_back({plot.L, yM});
        mHoldPts.push_back({plot.L, yH});
      }

      mSpecPtsL.push_back({x, yL});
      mSpecPtsR.push_back({x, yR});
      mSpecPtsM.push_back({x, yM});
      mHoldPts.push_back({x, yH});
    }

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

    float topY = plot.B;
    for (const Pt &p : pts)
      topY = std::min(topY, p.y);
    const IRECT gradRect(plot.L, topY, plot.R, plot.B);
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
  static constexpr float kSpecAnchorLo = 10.f;
  static constexpr float kSpecAnchorHz = 14.14214f; // sqrt(10·20)
  static constexpr int kSubBand = -2;
  static constexpr float kTopDb = 9.f;
  static constexpr float kVuTopDb = 3.f;
  static constexpr float kVuBottomVU = -20.f;
  static constexpr float kTickRight = 3.f;
  static constexpr float kLabelH = 16.f;

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

  std::vector<float> mSpectrum[3];
  float mPeakL = -120.f, mPeakR = -120.f;
  float mTrueL = -120.f, mTrueR = -120.f;
  float mRmsL = -120.f, mRmsR = -120.f;
  float mVuL = -120.f, mVuR = -120.f;
  float mVuHoldL = -120.f, mVuHoldR = -120.f;
  float mPersistL = -120.f, mPersistR = -120.f;
  float mHoldL = -1000.f, mHoldR = -1000.f;
  float mHoldSec = 2.f;
  int mMeterMode = 0;
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
  int mMode = 0;
  int mChanMode = 0;
  int mMergeAlgo = 0;
  std::vector<float> mVQTFreqs;
  std::vector<float> mVQTFreqNorm;
  std::vector<float> mPBTFreqs;
  std::vector<float> mPBTFreqNorm;
  std::vector<float> mRTAFreqs;
  std::vector<float> mRTAFreqNorm;
  std::array<float, kSpectrumBands> mBandNormX{};

  static constexpr float kSlopeRefHz = 632.45553f; // sqrt(20·20000)
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
  int mReleaseMode = 0;
  float mAttackSec = 0.05f;
  float mReleaseSec = 0.2f;
  float mBottomDb = -100.f;
  int mNumBins = 2048;
  double mSampleRate = 48000.0;

  std::vector<Pt> mSpecPtsL;
  std::vector<Pt> mSpecPtsR;
  std::vector<Pt> mSpecPtsM;
  std::vector<Pt> mHoldPts;

  std::vector<float> mHoldSpec[3];
  std::vector<float> mHoldAge[3];
  bool mHoldSignal = false;
  bool mHoldTpValid = false;
  bool mHoldWasActive = false;
  std::chrono::steady_clock::time_point mLastHoldTp{};

  bool mHoverActive = false;
  float mHoverX = 0.f;
  float mHoverY = 0.f;

  ILayerPtr mGridLayer;
  float mGridBottomDb = -1000.f;
  int mGridHue = -1;
  int mGridSat = -1;
  int mGridMode = -1;

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
