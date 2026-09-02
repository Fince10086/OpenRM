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
  // 尺寸必须与四个分析引擎的数据包严格一致 (SpectrumSTFT/VQT/PBT/RTA 均为 8192):
  // ISender 数据包整体拷贝, 这里按同尺寸结构体读取, 不一致会整包读取失败。
  using TDataPacket = std::array<float, 8192>;

  enum MsgTags {
    kMsgTagSampleRate = 1,
    kMsgTagFFTSize,
    kMsgTagRelease,
    kMsgTagReleaseMode, // 释放回落模式 (0: 对数域单极点, 1: 匀速 dB 速率)
    kMsgTagRange,
    kMsgTagSlope,       // 频谱斜率 (dB/oct, 当前模式生效值; FFT 与逐 band 引擎档值不同)
    kMsgTagMode,
    kMsgTagVQTBands,
    kMsgTagReset,       // 清空平滑缓冲, 显示从头加载 (γ/BPO/模式切换时由插件下发)
    kMsgTagChanMode,    // 声道显示模式 (0: L/R, 1: MERGE)
    kMsgTagMergeAlgo,   // 合并算法 (0: PWR 功率和, 1: SUM 单声道和)
    kMsgTagLevelMeter,  // 电平表数据 (LevelMeterUiData)
    kMsgTagLoudness,    // 响度数据 (LoudnessUiData, M/S 条用)
    kMsgTagPBTBands,    // PBT 频带中心频率 (Hz)
    kMsgTagRTABands,    // RTA 频带中心频率 (Hz)
  };

  // 电平条点击动作 (Analyzer.cpp 绑定回调转成 mLevelReset*Flag / 参数循环档位)
  enum EMeterClick {
    kClickResetPersist,   // dBTP 模式点击条: 清持久锁存
    kClickResetMeterHold, // dBFS 模式点击条体: 清峰值保持
    kClickResetOver,      // dBFS 模式点击顶部 LED: 清过载
    kClickLoudScale,      // 响度窗顶刻度按钮: 循环刻度窗偏移 (+9/+18 LU)
    kClickLoudPreset,     // 响度 1/3 目标刻度按钮: 循环目标预设 (-9/-14/-23/-24 LUFS)
  };
  std::function<void(EMeterClick)> mMeterClickHandler;

  SpectrumPad(const IRECT &bounds) : IControl(bounds) {
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
      mHoldSpec[c].assign(mHoldSpec[c].size(), -1000.f);
      mHoldAge[c].assign(mHoldAge[c].size(), 0.f);
    }
    mHoldSignal = false;
    SetDirty(false);
  }

  // 方案0 测量: 快捷键 P 切换绘制耗时 HUD (由 Analyzer 的全局按键处理调用)
  void ToggleHud() {
    mHudOn = !mHudOn;
    SetDirty(false);
  }

  void OnMsgFromDelegate(int msgTag, int dataSize, const void *pData) override {
    IByteStream stream(pData, dataSize);

    if (msgTag == ISender<>::kUpdateMessage) {
      ++mDataPkts; // 方案0: 数据帧分类计数
      ISenderData<3, TDataPacket> d;
      stream.Get(&d, 0);
      // FFT: 数据 = bins (nBins 个); VQT/PBT/RTA: 数据 = band 幅度 (nBands 个)
      const int nVals = (mMode == 0) ? std::max(mNumBins, 0)
                                     : (mMode == 1) ? (int)mVQTFreqs.size()
                                     : (mMode == 2) ? (int)mPBTFreqs.size()
                                                    : (int)mRTAFreqs.size();
      if (nVals <= 0)
        return;

      // 帧进给周期恒 1024 样本: FFT 档位 overlap 规则 (2048/2, 4096/4, 8192/8)
      // 保证 hop 恒 1024, 其余引擎 kHop 固定 1024 —— 平滑时间常数与引擎同步
      const double hop = 1024.0;
      const double updatePeriod = hop / std::max(mSampleRate, 1.0);
      mAttackCoeff = (float)std::exp(-updatePeriod / mAttackSec);
      mReleaseCoeff = (float)std::exp(-updatePeriod / mReleaseSec);
      // 匀速档: 每帧固定"屏高比例"下落 (2·τ 秒跨一屏) -> 显示域恒像素速度,
      // 与显示范围/斜率无关 (Pro-Q 式视觉恒速)
      const float unifStepDb = (float)(updatePeriod / (2.0 * std::max(mReleaseSec, 1e-3f)) * (kTopDb - mBottomDb));

      const float a = mAttackCoeff, r = mReleaseCoeff;
      if (mMode == 0) {
        ProcessFFTBands(d, a, r, unifStepDb);
      } else {
        // VQT/PBT/RTA: 逐 band 显示域弹道 (目标 = 幅度 dB + 斜率, 已钳到显示范围)。
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
              // PBT 物理起振时间常数 τ = 1 / (π · bw): 窄带低频展现自然蓄力爬坡感
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
      mReleaseSec = std::clamp(releaseSec, 0.01f, 10.f); // LIN 最慢档释放达 9.6s
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
      ++mDataPkts; // 方案0: 数据帧分类计数
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
      ++mDataPkts; // 方案0: 数据帧分类计数
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
      ++mDataPkts; // 方案0: 数据帧分类计数
      for (int c = 0; c < 3; ++c)
        mSpectrum[c].assign(mSpectrum[c].size(), -150.f);
      ClearPeakHold();
      SetDirty(false);
    }
  }

  // 点击交互 (按命中优先级): 响度刻度按钮 (窗顶值 → 循环刻度窗偏移 +9/+18;
  // 1/3 目标值 → 循环目标预设 -9/-14/-23/-24) > L/R 条区域 (dBTP → 清持久锁存;
  // dBFS → 顶部 LED 区清过载 / 下方条体清峰值保持)。模式按钮 attach 在后, 优先命中。
  // 不做按键位过滤 (与下方 L/R 条区旧逻辑一致, 整个控件按左键交互): 只按坐标命中,
  // 避免个别平台/事件路径的 IMouseMod.L 位语义差异导致按钮点击被放过。
  void OnMouseDown(float x, float y, const IMouseMod &mod) override {
    if (!mMeterClickHandler)
      return;
    // 响度 LUFS 刻度列的两个「刻度按钮」: 几何与 DrawLoudBars 同源 (LufsTickRect 外扩块,
    // 忽略像素对齐的亚像素差), 见 LoudScaleTickBtnRect / LoudPresetTickBtnRect。
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
    const float barL0 = mRECT.R - totalW; // 与 Draw 的 plot.R 一致 (忽略像素对齐的亚像素差)
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

  // hover 十字准线: 记录位置并请求重绘; 离开控件时清除 (仅显示, 不捕获鼠标)
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

  // 方案0 测量: 绘制 CPU 耗时采样 (steady_clock, 只含本控件绘制, 不含其它控件与帧提交)。
  // 计时外壳 + HUD; 实际绘制内容在 DrawContent。
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
    // 图形区左对齐, 右侧让出 L/R 两条电平表竖条 + VU 刻度区 + 独立 VU 表双条 +
    // 响度 M/S 条 (LUFS 刻度 + 双条)。对 plot 做物理像素对齐: 层位图/层内绘制/
    // 贴图/频谱/电平表共用同一矩形, 避免层内容与位图边缘之间的亚像素透明条带在
    // 右侧/底侧露出底色 (白边)。
    const IRECT plot = mRECT.GetReducedFromRight(kMeterStripW)
                           .GetPixelAligned(g.GetScreenScale() * g.GetDrawScale());

    // hover 准线分三个互不相通的刻度域 (刻度值不互通, 横向准线只贯穿本域):
    //   频谱 + L/R 电平条 (dBTP/dBFS): 共用同一 dB 映射 —— 横线从频谱左缘贯穿到
    //     L/R 条右缘, 一份 dB 动态读数 (频谱右缘, 与固定 dB 刻度避让);
    //   独立 VU 表: 横线只在本域内, 动态读数 = VU 值, 放在条左侧 VU 刻度列内
    //     (与固定刻度同位置同样式, 顶部放不下翻到线下侧);
    //   响度 M/S/I 条: 横线只在本域内, 动态读数 = LUFS 值 (目标锚定窗, 与条体
    //     刻度同一映射), 放在条左侧 LUFS 刻度列内。
    // VU/LUFS 动态读数直接以刻度样式文字绘制, 背景透明 (不垫任何芯片色块);
    // 文字框 (chip rect) 仅作避让几何: VU 读数与本域固定刻度重叠时隐藏该刻度让位
    // (与频谱 dB/Hz 刻度避让同策略); LUFS 读数与任一固定刻度 (顶/中刻度按钮、底
    // 刻度文字) 重叠时由下方平移逻辑把读数纵向让开, 固定刻度保持完整可见。
    // VU 表与响度表的 hover 只在各自的「条体」上触发: 鼠标进入刻度文字列 (VU/LUFS
    // 刻度列) 不激活准线/读数, 避免悬停在刻度上时读数闪烁。刻度列的「刻度按钮」
    // (窗顶值/目标值, 见 DrawLoudBars) 自带 hover 反馈, 不依赖本准线系统。
    // 竖线 + 频率/音高读数只在频谱区内; dBFS 模式下 L/R 条顶部 over LED 区内
    // 整组准线不显示。
    const float zoneLr = LrZoneR(plot); // 频谱+L/R 域右缘 (L/R 条右缘)
    const float zoneVu = VuZoneR(plot); // VU 域右缘 (VU 条右缘)
    const bool ledArea = (mMeterMode == 1) && (mHoverX > plot.R) &&
                         (mHoverX <= zoneLr) && (mHoverY < YOf(plot, 0.f) - 3.f);
    const bool inStrip = mHoverActive && !ledArea && mHoverX >= plot.L && mHoverX <= mRECT.R &&
                         mHoverY >= plot.T && mHoverY <= plot.B;
    const bool inSpectrum = inStrip && mHoverX <= plot.R;
    const bool inZone0 = inStrip && mHoverX <= zoneLr;
    // VU 条体 = 刻度列右缘 (VU 条左缘) 到 VU 域右缘; 刻度列 (VU 条左侧) 不算 hover
    const bool inZone1 = inStrip && mHoverX > LrZoneR(plot) + kVuScaleW && mHoverX <= zoneVu;
    // 响度条体 = LUFS 刻度列右缘 (响度条左缘) 之后; LUFS 刻度列不算 hover
    const bool inZone2 = inStrip && mHoverX > VuZoneR(plot) + kLufsScaleW;
    IRECT hzSkip, dbSkip, vuSkip, lufsSkip; // 默认空矩形 = 不跳过任何刻度
    char hzBuf[16] = "", dbBuf[16] = "";
    char vuBuf[16] = "", lufsBuf[16] = "";
    char noteBuf[8];   // 音高标签 (C4 等最近音名), inSpectrum 内计算
    IText hzText, dbText, vuText, lufsText;
    IRECT pitchR, freqR;    // 音高/频率标签绘制矩形, inSpectrum 内计算
    IRECT vuBox, lufsBox;   // VU/LUFS 动态读数文字框 (刻度列内, 刻度样式)
    IRECT vuChip, lufsChip; // VU/LUFS 动态读数避让矩形 (文字框外扩 2px, 不再填充背景)
    float xLine = 0.f, yLine = 0.f;
    if (inStrip) {
      xLine = mHoverX;
      yLine = mHoverY;

      if (inSpectrum) {
        // x -> 对数频率 (与 FreqNorm 反函数一致): 20Hz..20kHz
        const double hz = 20.0 * std::pow(1000.0, (double)(xLine - plot.L) / plot.W());
        FreqToNoteName(hz, noteBuf, sizeof(noteBuf));
        if (hz < 1000.0)
          std::snprintf(hzBuf, sizeof(hzBuf), "%.0f Hz", hz);
        else
          std::snprintf(hzBuf, sizeof(hzBuf), "%.2f kHz", hz / 1000.0);
      }

      if (inZone0) {
        // y -> dB (mBottomDb..kTopDb, 频谱与 L/R 条共用同一映射)
        const float db = mBottomDb + (kTopDb - mBottomDb) * (plot.B - yLine) / plot.H();
        std::snprintf(dbBuf, sizeof(dbBuf), "%.1f dB", db);
      } else if (inZone1) {
        // y -> VU (满高 -20..+3 VU, 与 VU 条同一映射); 读数放条左侧刻度列, 刻度样式
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
        // y -> LUFS (目标锚定窗, 与条体刻度同一映射); 读数放条左侧刻度列, 刻度样式
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
        // 动态读数 (文字框) 与任一固定刻度 (顶窗顶值/中目标值刻度按钮, 以及底部窗底值
        // 刻度文字) 重叠时, 把读数整体纵向平移让开 (优先上移, 越出条区则下移), 使三个
        // 固定刻度在任何 hover 下都完整可见 —— 按钮不会被读数文字压住而像「消失」,
        // 底部纯文字刻度也不会与读数叠字。平移间距 gap。
        // (按钮位置见 LoudScaleTickBtnRect/LoudPresetTickBtnRect, 与绘制同源。)
        {
          const float gap = 3.f;
          const IRECT btnObstacles[3] = {LoudScaleTickBtnRect(plot),
                                         (mTarget > -100.f) ? LoudPresetTickBtnRect(plot)
                                                            : IRECT(),
                                         LufsTickRect(plot, botL)};
          for (const IRECT &btn : btnObstacles) {
            if (btn.Empty() || !lufsChip.Intersects(btn))
              continue;
            // 上移: chip.B -> btn.T - gap; 不越出条区上缘
            float dy = (btn.T - gap) - lufsChip.B;
            if (lufsChip.T + dy < plot.T) {
              // 下移: chip.T -> btn.B + gap; 不越出条区下缘
              dy = (btn.B + gap) - lufsChip.T;
              if (lufsChip.B + dy > plot.B)
                continue; // 上下都放不下 (条区极矮), 放弃避让, 由让位逻辑兜底
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
      }

      if (inZone0) {
        // dB 标签: 与右侧 dB 刻度同布局同样式 (文字在线上方, 右对齐);
        // 顶部放不下时翻转到线下侧。
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
      // 准线颜色与刻度文字一致 (COL_700), 1px 细线; 最上层绘制
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
  // ---- 方案0 绘制耗时采样与 HUD ----

  // 采样一次 pad 绘制耗时; data/hover 帧分类 = 自上次绘制以来是否收到过数据包
  // (数据泵 ~20Hz 触发 data 帧; 60Hz 渲染门在数据间隙触发 hover 帧)
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

  // 绘制耗时 HUD (绘图区左下角, 两行小字; 快捷键 P 开关):
  //   pad <avg> ms avg / <max> max / <N> draw/s
  //   data <avg> ms x<N>/s | hover <avg> ms x<N>/s
  // 窗口 = 最近 120 次 pad 绘制 (~2 s @60fps); 方案1 落地前后的对比基线。
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

  // 电平表满刻度 (显示顶部): dBTP +6 dB / dBFS 0 dBFS。
  // 刻度网格仍固定到 kTopDb, 顶部空出的区域放置 over LED。
  float MeterTopDb() const {
    return (mMeterMode == 1) ? 0.f : kTopDb;
  }

  float YOf(const IRECT &plot, float db) const {
    return plot.B - (db - mBottomDb) / (kTopDb - mBottomDb) * plot.H();
  }

  // ── hover 三域几何与固定刻度矩形 ──────────────────────────────────────
  // 电平条带分三个互不相通的刻度域: 频谱+L/R 条 (共用 dB 映射, 相互贯穿) |
  // 独立 VU 表 | M/S/I 响度条。固定刻度与 hover 动态读数共用同一矩形公式,
  // 保证避让与位置永远同源。
  float LrZoneR(const IRECT &plot) const { return plot.R + 2.f * kGainBarW; }
  float VuZoneR(const IRECT &plot) const { return LrZoneR(plot) + kVuScaleW + 2.f * kVuBarW; }

  // VU 刻度文字矩形 (DrawVuScale 同式; +3 VU 贴条顶翻到线下侧)
  IRECT VuTickRect(const IRECT &plot, float vu) const {
    const float scaleL = LrZoneR(plot);
    if (vu >= kVuTopDb)
      return IRECT(scaleL, plot.T + 1.f, scaleL + kVuScaleW - kTickRight, plot.T + 1.f + kLabelH);
    const float y = plot.B - (vu - kVuBottomVU) / (kVuTopDb - kVuBottomVU) * plot.H();
    return IRECT(scaleL, y - kLabelH - 1.f, scaleL + kVuScaleW - kTickRight, y - 1.f);
  }

  // 响度条目标锚定刻度窗: 顶/底 LUFS (目标无效退回固定 -60..0 旧制)
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

  // LUFS 刻度文字矩形 (DrawLoudBars 同式; 顶刻度贴条顶翻到线下侧)
  IRECT LufsTickRect(const IRECT &plot, float lufs) const {
    const float scaleL = VuZoneR(plot);
    float topL, botL;
    LoudWindow(topL, botL);
    if (lufs >= topL)
      return IRECT(scaleL, plot.T + 1.f, scaleL + kLufsScaleW - kTickRight, plot.T + 1.f + kLabelH);
    const float y = LoudYOf(plot, lufs);
    return IRECT(scaleL, y - kLabelH - 1.f, scaleL + kLufsScaleW - kTickRight, y - 1.f);
  }

  // ── 响度刻度按钮 (窗顶值 / 1/3 目标值) ───────────────────────────────
  // 响度 LUFS 刻度列的顶刻度 (窗顶值 = 目标+偏移) 与 1/3 刻度 (目标值) 兼作按钮:
  // 文字保持普通刻度的位置/字体/颜色, 仅垫一块浅灰底 (COL_300), 点击分别循环
  // 刻度窗偏移 (+9/+18, 见 kClickLoudScale) 与目标预设 (-9/-14/-23/-24, 见
  // kClickLoudPreset)。热区 = 刻度文字矩形上下外扩 1px (左右不越出刻度列), 与
  // DrawLoudBars 的绘制同源 (LufsTickRect), OnMouseDown 与 hover 高亮共用。
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

  // L/R 双电平表条 + 独立 VU 表 (VU 刻度文字画在两者之间的间距内) + over 指示
  // (dBTP 锁存数字直接绘制在条上, 见 DrawMeterBar)
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

  // 独立 VU 表刻度文字: -20/-10/0/+3 (VU), 右对齐 VU 表左缘, 画在 L/R 条与 VU 表
  // 之间的间距内。样式与 dB 刻度一致 (文字在线上方); +3 贴条顶放不下, 翻到线下侧。
  // skipRect 非空时 (hover 动态读数), 重叠的固定刻度隐藏让位。
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

  // 独立 VU 表双条 (L/R): 常驻显示两声道功率和 VU (0 VU = -18 dBFS), 独立刻度 -20..+3 VU。
  // 渐变 = 两段语义色: -20..-3 VU 深绿→浅绿 (安全区), -3..+3 VU 浅绿→红 (逼近削波,
  // 0 VU 附近自然呈黄绿)。峰值保持线用 VU 表自己的保持值 (mVuHold), 与 L/R 条
  // (mHold) 相互独立, 同样按 VU 位置取色。双条底部画 "VU" 水印标识 (见函数尾)。
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
      {0.f, 0, 0.72f, 0.93f},          // +3 VU 红 (顶)
      {tOfV(-3.f), 120, 0.55f, 0.78f}, // -3 VU 浅绿 (上区下界)
      {1.f, 150, 0.62f, 0.40f},        // -20 VU 深绿 (底)
    };
    const int nStops = (int)(sizeof(stops) / sizeof(stops[0]));

    auto meterColor = [&](const MeterStop &st, int alpha) {
      const float s = std::clamp(st.s * satScale, 0.f, 1.f);
      const IColor c = HSBToIColor(st.h, s, st.b);
      return IColor(alpha, c.R, c.G, c.B);
    };

    // 渐变停止点间按 t 插值 (t: 0=顶部 +3 VU, 1=底部 -20 VU; 与 VU 值的映射见 tOfV)
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

    // 渐变在 VU 位置的颜色 (与条体渐变一致, 停止点间 RGB 线性插值)
    auto meterColorAt = [&](float vu, int alpha) { return colorAtT(tOfV(vu), alpha); };

    // 单条绘制: 背景 + 渐变填充 + 峰值保持线 (各声道用各自的 VU 值与保持值)
    auto drawBar = [&](const IRECT &bar, float vuDb, float vuHold) {
      g.FillRect(COL_300(), bar);

      // 条体渐变填充: 按 stop 分段的 2-stop 渐变 (NanoVG 后端不支持多 stop 渐变, 见
      // DrawMeterBar); 锚定在段的名义跨度上, 段间重叠 1 设备像素消除抗锯齿接缝。
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

      // vuDb 为 dBFS 域 (0 VU = -18 dBFS), 先转 VU 域 (＋18) 再按 -20..+3 VU 刻度映射,
      // 使 -18 dBFS 信号落在 0 VU (刻度 87% 处)
      const float vu = vuDb + 18.f;
      const float yTop = yOfV(std::clamp(vu, kVuBottomVU, kVuTopDb), bar);
      fillGrad(yTop, 255);

      // 峰值保持线: vuHold 为 dBFS 域, 同样转 VU 域后按 VU 刻度取色
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

    // "VU" 水印标识 (纯文字, 非按钮, 无 hover, 透明背景): 位于 VU 双条底部, 与 L/R 条
    // 底部的 dBTP/dBFS 幽灵文字同一水平带 (条底 19px 高, 与 Analyzer.cpp 的 kRangeBtnH
    // 对齐)、同字体字号 (11px kFontSemiBold, Center/Middle) 居中于双条。文字色随条上
    // 渐变状态变化 (与 dBTP 幽灵文字同一观感逻辑): 空轨 (VU 值 ≤ 显示范围底部 -20 VU,
    // 无渐变渲染, 轨道呈浅灰) 时为深灰 COL_700 可辨; 有渐变时退回条轨浅灰 COL_300
    // 水印, 不抢戏。透明背景, hover 无任何反应。
    const float vuMaxDb = (mVuL > mVuR) ? mVuL : mVuR; // VU 数据为 dBFS 域 (0 VU = -18 dBFS)
    const IColor vuLblCol = (vuMaxDb + 18.f > kVuBottomVU) ? COL_300() : COL_700();
    const IText vuLbl(11.f, vuLblCol, kFontSemiBold, EAlign::Center, EVAlign::Middle);
    g.DrawText(vuLbl, "VU", IRECT(barL.L, plot.B - 19.f, barR.R, plot.B));
  }

  // 语义色降饱和 (固定 RGB 安全色随主题饱和档位; 纯黑白主题下变灰阶; 与声像仪表行同式)
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

  // 响度三条 (LUFS): 位于 VU 条右侧, 样式与 VU 条一致 (轨道 + 渐变段 + 顶部字母标签)。
  // 排列 (左→右) = I | S | M: I 为 2 倍宽强调 (2×kLoudBarW), S/M 各 kLoudBarW,
  // 总宽 4×kLoudBarW (kMeterStripW 相应 +1 份, 见 UiUtils.h)。M/S 数值显示已从右栏
  // 响度读数面板移除 (见 LoudnessMeterControl), 此处三根条即 M/S/I 唯一的实时条体。
  // 竖向刻度以目标为锚 (kMsgTagLoudness 随帧下发的 target + scaleOff):
  //   1/3 高度 = 目标值, 顶部 = 目标+偏移, 底部 = 目标-2×偏移 (窗高 = 3×偏移);
  //   目标无效时退回固定 -60..0 旧制。顶部刻度贴条顶翻到线下侧 (与旧 0 刻度同处理)。
  // 语义色改为目标相对 (与 Δ 读数同三段式): ≤目标-1 黄 (偏安静) / |Δ|≤1 绿 (达标) /
  //   ≥目标+1 红 (偏响); 颜色走 satScale 降饱和, 纯黑白主题下自动变灰阶。
  // skipRect 非空时 (hover 动态读数), 重叠的固定刻度隐藏让位 —— 仅兜底:
  // LUFS 侧读数已在 DrawContent 平移避开全部固定刻度, 正常情形不触发。
  // 顶刻度 (窗顶值) 与 1/3 刻度 (目标值) 兼作「刻度按钮」: 文字位置/字体/颜色与普通
  // 刻度完全一致 (底刻度仍为纯文字), 无常驻底色 —— hover 时只在文字背后垫一层
  // 半透明加深遮罩提示可点; 点击分别循环刻度窗偏移 (+9/+18) 与目标预设
  // (-9/-14/-23/-24, 见 OnMouseDown)。读数与按钮重叠时由 DrawContent 将读数
  // 平移让开 (读数优先可读), 按钮保持完整可见可点, 不参与让位隐藏。
  // 三条底部的水印 (同 VU 水印样式, 见函数尾): I 条下显示 I 当前值; S 条下 "LU" 与
  // M 条下 "FS" 拼成单位 "LUFS", 两段各自随所属条的渐变状态取色 —— 纯文字透明背景
  // 无 hover。
  void DrawLoudBars(IGraphics &g, const IRECT &plot, const IRECT &skipRect) {
    const float scaleL = VuZoneR(plot);
    const float bar0L = scaleL + kLufsScaleW;
    // I 最左 (2 倍宽) | S 中 | M 右
    const IRECT barI(bar0L, plot.T, bar0L + 2.f * kLoudBarW, plot.B);
    const IRECT barS(barI.R, plot.T, barI.R + kLoudBarW, plot.B);
    const IRECT barM(barS.R, plot.T, barS.R + kLoudBarW, plot.B);

    float topL, botL;
    LoudWindow(topL, botL);
    const bool tgtValid = mTarget > -100.f;

    // 刻度文字样式 (与底刻度 / hover 动态读数一致)
    const IText t(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Bottom);
    auto drawTickText = [&](float v, const IRECT &labelR) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "%d", (int)std::lround(v));
      g.DrawText(t, buf, labelR);
    };
    auto hiddenByChip = [&](const IRECT &labelR) {
      return !skipRect.Empty() && labelR.Intersects(skipRect);
    };
    // 刻度按钮: 无常驻底色, 按钮身份只由 hover 呈现 —— 悬停时在文字背后垫一层
    // 半透明加深遮罩 (HoverOverlay, 不染色整列), 文字保持原刻度样式/颜色不变。
    auto drawTickBtn = [&](float v, const IRECT &labelR) {
      const IRECT btn = LoudTickBtnRect(labelR);
      if (mHoverActive && btn.Contains(mHoverX, mHoverY))
        g.FillRect(HoverOverlay(), btn);
      drawTickText(v, labelR);
    };

    // 顶刻度按钮 (窗顶值 = 目标+偏移, 点击循环刻度窗偏移 +9/+18)
    const IRECT topLabel = LufsTickRect(plot, topL);
    if (!hiddenByChip(topLabel))
      drawTickBtn(topL, topLabel);

    // 1/3 目标刻度按钮 (目标值, 点击循环目标预设 -9/-14/-23/-24); 目标无效时不显示
    if (tgtValid) {
      const IRECT midLabel = LufsTickRect(plot, mTarget);
      if (!hiddenByChip(midLabel))
        drawTickBtn(mTarget, midLabel);
    }

    // 底刻度 (窗底值 = 目标-2×偏移): 保持纯刻度文字
    const IRECT botLabel = LufsTickRect(plot, botL);
    if (!hiddenByChip(botLabel))
      drawTickText(botL, botLabel);

    // 目标线 (跨三条 I|S|M), 落在 1/3 高度刻度处
    if (tgtValid) {
      const float yT = LoudYOf(plot, mTarget);
      g.FillRect(COL_900(), IRECT(barI.L, yT - 1.f, barM.R, yT + 1.f));
    }

    // 语义色段 (目标相对, 与 Δ 读数同三段式; satScale 降饱和)
    struct LStop { float lufs; IColor c; };
    const LStop stops[] = {
      {botL, SemColor(MeterYellow())}, // ≤ 目标-1 偏安静
      {mTarget - 1.f, SemColor(MeterGreen())},
      {mTarget + 1.f, SemColor(MeterRed())},
      {topL, SemColor(MeterRed())},    // ≥ 目标+1 偏响
    };
    const int nStops = (int)(sizeof(stops) / sizeof(stops[0]));
    const float seamOv = 1.f / std::max(1.f, g.GetScreenScale() * g.GetDrawScale());

    // 单条: 轨道 + 分段 2-stop 渐变填充 (NanoVG 后端不支持多 stop, 见 DrawMeterBar) + 顶部小标签
    auto drawBar = [&](const IRECT &bar, float lufs, const char *label) {
      g.FillRect(COL_300(), bar);
      if (lufs > -99.f) {
        const float yTop = LoudYOf(plot, lufs);
        // stops 按 lufs 从底 (目标-2·off) 到顶 (目标+off) 排列,
        // 因此 yA = 段的下缘 (大 y), yB = 段的上缘 (小 y);
        // 只在信号电平以下绘制: 段整体在信号之上 (下缘 ≤ yTop) 时跳过。
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

    // ── 底部水印 (样式同 VU 水印, 见 DrawVuBar 尾) ────────────────────────
    // 纯文字透明背景, 无 hover, 画在条底 19px 水平带 (与 dBTP 幽灵文字/VU 水印同带,
    // Analyzer.cpp kRangeBtnH); 颜色随条上渐变双态: 值超出窗底 (条上渲染了渐变) 呈
    // 条轨浅灰 COL_300 水印, 空轨/无效呈深灰 COL_700。I 条下 = I 当前具体值 (%.1f,
    // 无效 "—"); 单位 "LUFS" 拆成两段 —— "LU" 落在 S 条下、"FS" 落在 M 条下,
    // 分别随 S (mShortTerm) 与 M (mMomentary) 的渐变状态独立取色 (如 S 有渐变
    // M 空轨 → LU 浅灰、FS 深灰)。文字先以 11px 试排, MeasureText 越出归属条域时
    // 逐级降字号 (两字母在 14px 条内通常降至 ~10px, 与顶部字母标签同字号观感)。
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
    // 底值与 botL 已由上方 LoudWindow 解出 (目标无效时退回固定 -60 窗底), 恒适用
    const bool iAct = loudMarkActive(mIntegrated);
    const bool sAct = loudMarkActive(mShortTerm);
    const bool mAct = loudMarkActive(mMomentary);
    char iBuf[16];
    if (mIntegrated > -99.f)
      std::snprintf(iBuf, sizeof(iBuf), "%.1f", mIntegrated);
    else
      std::snprintf(iBuf, sizeof(iBuf), "%s", "—");
    drawLoudMark(barI, iBuf, iAct);
    // 单位 "LUFS" 拆成两段, 各自落在 S / M 条底并分别取色: "LU" 随 S 条渐变状态
    // (sAct), "FS" 随 M 条 (mAct)。例: S 有渐变柱子而 M 空轨 → LU 浅灰、FS 深灰。
    drawLoudMark(barS, "LU", sAct);
    drawLoudMark(barM, "FS", mAct);
  }

  // ── LRA bracket (Pro-L2 模仿) ────────────────────────────────────────────
  // 位于 M 条右侧的 32px 区域 (kLraZoneW), 仅当 mLraValid (≥3s, 首个 3s 块后) 时绘制。
  // 几何:
  //   spine  = 垂直线, 贴 M 条右缘 (x = strip 右缘), 跨越 LRA 高/低两点 LUFS 对应的 y
  //   caps   = 上/下两段水平短线, 从 spine 向右伸出 (Pro-L2 反向风格: bracket 朝向条之外)
  //   文字   = "LRA" 小标 + "XX.X" 大值, 竖排两行, 右对齐 cap 右端外
  // 上/下臂 y = LoudYOf(plot, lraMax / lraMin), 即 EBU 3342 直方图 95%/10% 百分位 LUFS;
  // 不在可见窗内则夹到 [botL, topL]。无 hover/无交互, 颜色走 SemColor(MeterYellow())
  // (与 VU/LUFS 条体同降饱和链路, 主题黑白模式自动灰化)。
  void DrawLraBracket(IGraphics &g, const IRECT &plot) {
    if (!mLraValid)
      return;
    float topL, botL;
    LoudWindow(topL, botL);
    // LoudYOf: LUFS 越高 y 越小 (靠上) → lraMax (95% 上臂) 得 yTop, lraMin (10% 下臂) 得 yBot
    const float yTop = LoudYOf(plot, std::clamp(mLraMax, botL, topL));
    const float yBot = LoudYOf(plot, std::clamp(mLraMin, botL, topL));
    if (yBot - yTop < 1.f)
      return; // 范围太小, 上下臂重合, 不画

    const float zoneL = plot.R + kMeterStripW - kLraZoneW; // = M 条右缘 = spine 起点
    const float zoneR = plot.R + kMeterStripW;             // = 频谱面板右缘 = 文字右边界
    const IColor col = SemColor(MeterYellow());

    // spine + caps (1.5px 粗细; 上下臂居中位于 yTop / yBot)
    constexpr float kSpineW = 1.5f, kCapH = 1.5f, kCapLen = 6.f;
    g.FillRect(col, IRECT(zoneL, yTop, zoneL + kSpineW, yBot));
    g.FillRect(col, IRECT(zoneL, yTop - kCapH * 0.5f, zoneL + kCapLen, yTop + kCapH * 0.5f));
    g.FillRect(col, IRECT(zoneL, yBot - kCapH * 0.5f, zoneL + kCapLen, yBot + kCapH * 0.5f));

    // 文字: "LRA" (小标) + "XX.X" (值), 竖排, 右对齐 zoneR。字号随跨度缩放,
    // 两行整体居中于上下臂之间; 空间不足时贴顶 (允许向下微溢)。
    char valBuf[16];
    std::snprintf(valBuf, sizeof(valBuf), "%.1f", mRange);
    const float midY = (yTop + yBot) * 0.5f;
    const float span = yBot - yTop;
    const float labelSize = std::clamp(span * 0.28f, 9.f, 12.f); // "LRA" 字号随跨度缩
    const float valSize = std::clamp(span * 0.42f, 11.f, 16.f);  // "XX.X" 字号随跨度缩
    const float gap = 1.f;
    const float labelH = labelSize + 2.f;
    const float valH = valSize + 2.f;
    const float totalH = labelH + gap + valH;
    const float topLimit = std::max(yTop + 1.f, yBot - totalH - 1.f); // 防 clamp 界翻转
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

    // 条色随主题饱和档位的映射 (L/R 电平条专用, 锚点分段线性):
    //   低 (0)   → ×0  去饱和灰阶 (原有);
    //   中 (15)  → ×0.25 (旧 sat≈7.5 观感, 介于旧低与旧中之间);
    //   高 (50)  → ×0.5 (旧中档观感, 档位拉满也不再艳过默认)。
    // 沿用旧公式的话中档 ×0.5 已显鲜艳, 此处整体下调一档。
    const int satClamp = std::min(ThemeSatMax(), 50);
    const float satScale = (satClamp <= 15) ? ((float)satClamp / 60.f)
                                            : (0.25f + (float)(satClamp - 15) / 140.f);

    // 渐变停止点 (t: 0=顶部 +9 dB, 1=底部), 条体与峰值保持线共用同一组颜色。
    // 饱和度整体下调 (高饱和区段降 0.07~0.08), 低饱和区段 (绿/青/底部) 亮度明显
    // 压暗 (b 降 0.04~0.09), 让大片底色不刺眼、整体更沉稳。
    struct MeterStop { float t; int h; float s; float b; };
    const MeterStop stops[] = {
      {0.f, 0, 0.73f, 0.87f},          // +9 dB 红
      {tOf(0.f), 4, 0.71f, 0.92f},     // 0 dB 橙红
      {tOf(-6.f), 32, 0.78f, 0.94f},   // -6 dB 黄
      {tOf(-14.f), 50, 0.73f, 0.86f},  // -14 dB 黄绿
      {tOf(-24.f), 140, 0.58f, 0.71f}, // -24 dB 绿
      {tOf(-48.f), 150, 0.68f, 0.60f}, // -48 dB 青
      {1.f, 156, 0.70f, 0.48f},        // 底部 蓝绿
    };
    const int nStops = (int)(sizeof(stops) / sizeof(stops[0]));

    auto meterColor = [&](const MeterStop &st, int alpha) {
      const float s = std::clamp(st.s * satScale, 0.f, 1.f);
      const IColor c = HSBToIColor(st.h, s, st.b);
      return IColor(alpha, c.R, c.G, c.B);
    };

    // 渐变停止点间按 t 插值 (t: 0=顶部, 1=底部; 与 dB 的映射见 tOf)
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

    // 渐变在 dB 位置的颜色 (与条体渐变一致, 停止点间 RGB 线性插值)
    auto meterColorAt = [&](float db, int alpha) { return colorAtT(tOf(db), alpha); };

    // 条体渐变填充: 按 stop 分段的 2-stop 渐变。IGraphics 的 NanoVG 后端只支持双色
    // 线性渐变 (多 stop IPattern 的中间停止点会被丢弃, 此前整条多 stop 渐变被渲染成
    // 首尾两色混色、色带不随刻度走)。每段色区单独一个引擎原生渐变, 锚定在段的
    // 名义跨度上, 使 stop 颜色精确落在对应 dB 位置 (与峰值保持线同源); 段间重叠
    // 1 设备像素, 消除相邻填充抗锯齿羽化造成的接缝细线。
    const float seamOv = 1.f / std::max(1.f, g.GetScreenScale() * g.GetDrawScale());
    auto fillGrad = [&](float yTop, int alpha) {
      if (yTop >= bar.B)
        return;
      for (int i = 0; i + 1 < nStops; ++i) {
        const float yA = plot.T + stops[i].t * plot.H();
        const float yB = plot.T + stops[i + 1].t * plot.H();
        if (yB <= yTop)
          continue; // 整段位于信号电平之上, 不绘制
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

    // 峰值保持线 (颜色与条体在该位置的实际显示颜色一致: dBFS 模式下条体是
    // 90 alpha 渐变叠加在 COL_300 背景上, 先按相同比例与背景合成再画, 避免偏亮偏艳)
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

    // dBTP 持久锁存显示 (两声道合并的总最大值): 真峰值越过 0 dBFS 后锁存, 只增不减,
    // 点击条重置; 取代旧的长久保持红色横条与图例行 dBTP 读数 (原 ChannelLegendControl)。
    // 以条顶同色 (同饱和度) 的单个小数字标注在锁存值对应高度 (0~+6 dB 顶部区),
    // 水平居中于两条之上; 个位数带一位小数, 两位数只显示整数。只在 L 条通道绘制一次。
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
        constexpr float kPersistLblH = 17.f; // 14px 字行高 (标签中点锚在锁存高度)
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

  // ── 显示域弹道 (mSpectrum 存显示 dB: 已含斜率, 范围归一) ──────────────
  // 弹道在"显示域"运行: 目标 = 原始幅度 dB + 斜率, 并钳到显示范围 [mBottomDb, kTopDb]
  // (屏外目标按屏底/屏顶处理)。由此:
  //   * 屏上运动轨迹只由弹道参数决定, 与显示范围(Range)、斜率、信号电平无关;
  //   * 各频段 (含斜率差) 的显示差距按同一比例收缩, 回落时频谱整体水平下沉, 无吊尾;
  //   * 目标钳屏内后, 回落天然收敛在屏内, 不再有冲出屏外/低电平跳变的问题。

  // 弹道目标: 原始 dB + 斜率, 钳到显示范围。底部带 ~10% 屏高缓冲 (Pro-Q 式:
  // 目标在屏底下方一点, 屏内最后一段的差距含缓冲, 不会因渐近屏底而拖尾磨蹭)
  float SmoothTarget(float rawDb, float tiltDb) const {
    const float over = 0.1f * (kTopDb - mBottomDb);
    return std::clamp(rawDb + tiltDb, mBottomDb - over, kTopDb);
  }

  // 单点弹道: 攻击 dB 域单极点; 回落按模式:
  //   LOG  = 显示域差距等比收缩 (与范围/斜率无关, 先快后慢)
  //   UNIF = 恒定屏高比例速率 (2·τ 跨一屏, 恒像素速度), 不低于目标
  float StepSmoothed(float prevDb, float targetDb, float aCoef, float rCoef, float unifStepDb) const {
    if (targetDb > prevDb)
      return aCoef * prevDb + (1.f - aCoef) * targetDb;
    if (mReleaseMode == 1)
      return std::max(targetDb, prevDb - unifStepDb);
    return rCoef * prevDb + (1.f - rCoef) * targetDb;
  }

  // 合并显示 dB: PWR = 功率和, SUM = 幅度和 (与旧幅度域合并公式同语义)
  static float MergeDb(float dL, float dR, int algo) {
    if (algo == 0) {
      const float p = std::exp2f(dL * 0.33219280949f) + std::exp2f(dR * 0.33219280949f); // 10^(dB/10)
      return 3.01029995664f * orm::FastLog2(p);
    }
    const float a = std::exp2f(dL * 0.16609640474f) + std::exp2f(dR * 0.16609640474f); // 10^(dB/20)
    return 6.02059991328f * orm::FastLog2(a);
  }

  // FFT 模式: bin -> 256 band 聚合 (取 max, 仅首带入场外推) 后做显示域弹道;
  // 无 bin 的空桶不参与绘制 (mBandUsed 门控), 曲线在真实 band 点间由贝塞尔插值。
  // 弹道在 band 域进行: 聚合后的点数只有 256, 计算量远小于原 bin 域逐点平滑。
  void ProcessFFTBands(ISenderData<3, TDataPacket> &d, float a, float r, float unifStepDb) {
    if ((int)mBinToBand.size() != mNumBins)
      RebuildBinToBand();
    const int nb = std::min(mNumBins, (int)d.vals[0].size());
    if (mSpectrum[0].size() != (size_t)kSpectrumBands)
      for (int c = 0; c < 3; ++c)
        mSpectrum[c].assign(kSpectrumBands, -150.f);

    // 每帧聚合缓冲 (栈上, 3×256)
    std::array<std::array<float, kSpectrumBands>, 3> bandMax;
    std::array<float, 3> anchor{};
    for (int c = 0; c < 3; ++c)
      bandMax[c].fill(0.f);
    mBandUsed.fill(false);
    bool anchorUsed = false;

    for (int i = 0; i < nb; ++i) {
      const int b = mBinToBand[i];
      if (b == kSubBand) { // 20Hz 下方锚点 (轴外位置, 只用于入场线斜率)
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

    // 空桶策略 (显示端插值): 低频 band 可窄于 bin 间距而完全无 bin, 这类空桶
    // 不造值 —— 弹道留在空桶 (目标在屏底外), 绘制时跳过, 由贝塞尔曲线直接在
    // 真实测量的 band 点之间插值相连 (旧版"空桶跳过、曲线直连"观感)。
    // 仅首带之前 (首个有 bin 的 band 以下) 外推入场线: 有 20Hz 下方锚点
    // (10-20Hz 桶) 时按 dB-对数频率线性内插带自然入场斜率, 否则首带常值。
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
      // 入场段只画左缘一点 (band 0): 外推值在 dB-对数频率轴上线性共线,
      // 若整段都画, 贝塞尔经过共线点只会渲染成直线, 在首带处形成折角。
      // 只留一点后, 该点与首带之间由贝塞尔弧线平滑相连 (拐向由真实曲线形状决定)。
      mBandUsed[0] = true;
    }

    // 显示域弹道 (含斜率)
    const bool hasTilt = !mSlopeDb.empty();
    for (int c = 0; c < 3; ++c)
      for (int b = 0; b < kSpectrumBands; ++b) {
        const float rawDb =
            (bandMax[c][b] > 1e-30f) ? 6.02059991328f * orm::FastLog2(bandMax[c][b]) : -150.f;
        const float target = SmoothTarget(rawDb, hasTilt ? mSlopeDb[b] : 0.f);
        mSpectrum[c][b] = StepSmoothed(mSpectrum[c][b], target, a, r, unifStepDb);
      }
  }

  // 频谱峰值保持: 开关/时长与电平表 hold 共用 (mHoldSec 随电平表数据帧透传, 0 = 关,
  // ∞ 档为 1e9)。逐点跟踪平滑后显示 dB (与 mSpectrum 同域同长):
  // 刷新峰值即清计时; 超时时长后按恒定 20 dB/s 回落, 下限为当前显示值。
  // 帧间 dt 取 steady_clock 实测, 首帧/挂起恢复后只采峰不回落。
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

    // 回落速率 20 dB/s; ∞ 档恒为 0, 计时照常累加但永不超时
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

  // hold 曲线单点值 (显示 dB): LR 显示取双通道较大者, MERGE 用与显示曲线相同的 merge 公式
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

    // 通道色: L/R 基于主题色相 ±120°, 各自保持自身颜色 (不因重叠切换)。
    // 取色逻辑见 Theme.h GetChannelColors, 与色块图例保持一致。
    IColor cL, cR, cO;
    GetChannelColors(cL, cR, cO);

    // mSpectrum 已存显示 dB (含斜率, 弹道已钳在显示范围内), 直接映射像素
    const float span = kTopDb - mBottomDb;
    auto dbToY = [&](float db) -> float {
      return plot.B - (db - mBottomDb) / span * plot.H();
    };

    // 每 band 的 x 与曲线平滑策略 (PBT 折线, 其余贝塞尔)
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
        continue; // FFT 空桶不画点: 曲线在真实 band 点间由贝塞尔插值相连
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

    // 无信号时弹道目标停在屏底下方的 overshoot 缓冲带内 (mBottomDb - over),
    // 曲线整段在 plot.B 之下; 此时仍栅格化填充/描边, 会在绘图区底缘沿
    // plot.B 残留 ~1px 的水平线 (闭合边/描边的抗锯齿余迹)。整条曲线都低于
    // 底缘时跳过绘制, 曲线藏到屏外, 与其余时刻行为一致。
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
  static constexpr float kVuTopDb = 3.f;        // 独立 VU 表顶部 (+3 VU)
  static constexpr float kVuBottomVU = -20.f;   // 独立 VU 表底部 (-20 VU, 0 VU = -18 dBFS)
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

  std::vector<float> mSpectrum[3]; // 平滑后的显示 dB (已含斜率; FFT = 256 band, 其余 = band 数)
  float mPeakL = -120.f, mPeakR = -120.f;   // 电平表: 样本峰值 dBFS (已平滑)
  float mTrueL = -120.f, mTrueR = -120.f;   // 电平表: dBTP 真峰值 (已平滑)
  float mRmsL = -120.f, mRmsR = -120.f;     // 电平表: RMS dBFS (300ms 积分)
  float mVuL = -120.f, mVuR = -120.f;       // 电平表: VU 对应 dBFS (0 VU = -18 dBFS), 常驻
  float mVuHoldL = -120.f, mVuHoldR = -120.f; // 独立 VU 表峰值保持 (dBFS 域; -1000 = 无效)
  float mPersistL = -120.f, mPersistR = -120.f; // dBTP 持久锁存 (真峰值 > 0, 只增不减; -120 = 未触发)
  float mHoldL = -1000.f, mHoldR = -1000.f; // 电平表: 峰值保持 (显示域 dB, -1000 = 无效)
  float mHoldSec = 2.f;                     // 电平表: 保持时长 (s)
  int mMeterMode = 0;                       // L/R 条模式: 0=dBTP, 1=dBFS+RMS (独立 VU 表常驻)
  bool mOverL = false, mOverR = false;      // 电平表: 过载锁存
  float mMomentary = -120.f, mShortTerm = -120.f; // 响度 M/S (LUFS)
  float mIntegrated = -120.f;               // 总响度 I (LUFS)
  float mRange = 0.f;                        // LRA, LU (kMsgTagLoudness 随帧下发; DrawLraBracket 显示)
  float mLraMin = -120.f, mLraMax = -120.f;  // LRA 直方图 10%/95% LUFS 端点 (bracket 上下臂 y 映射)
  bool mLraValid = false;                    // LRA 可显示 (≥3s; 统计参考需 60s)
  float mTarget = -14.f;                    // 响度目标 (LUFS)
  float mScaleOff = 9.f;                    // 响度条刻度窗偏移 (LU, kMsgTagLoudness 随帧透传; 顶=目标+off, 1/3=目标, 底=目标-2·off)
  std::vector<int> mBinToBand;     // 预计算: bin -> band 映射 (-1 = 频段外)
  std::array<bool, kSpectrumBands> mBandUsed{}; // FFT: 本帧有 bin 的 band 标记 (含首带入场线), 绘制门控
  int mMode = 0;                   // 分析模式: 0=FFT, 1=VQT, 2=PBT, 3=RTA
  int mChanMode = 0;               // 声道显示模式: 0=L/R, 1=MERGE
  int mMergeAlgo = 0;              // 合并算法: 0=PWR 功率和, 1=SUM 单声道和
  std::vector<float> mVQTFreqs;     // VQT band 中心频率 (Hz), 由插件下发
  std::vector<float> mVQTFreqNorm;  // VQT band 频率归一化位置 (预计算, 与 mVQTFreqs 同步)
  std::vector<float> mPBTFreqs;     // PBT band 中心频率 (Hz), 由插件下发
  std::vector<float> mPBTFreqNorm;  // PBT band 频率归一化位置 (预计算, 与 mPBTFreqs 同步)
  std::vector<float> mRTAFreqs;     // RTA band 中心频率 (Hz), 由插件下发
  std::vector<float> mRTAFreqNorm;  // RTA band 频率归一化位置 (预计算, 与 mRTAFreqs 同步)
  std::array<float, kSpectrumBands> mBandNormX{}; // FFT 256 band 频率归一化位置 (预计算)

  // ── 频谱斜率 (显示域变换, 在弹道前施加) ────────────────────────────────
  // 每显示值 t 加常数 dB: tiltDb(f) = S·log2(f/f_pivot), S 为当前模式生效斜率
  // (FFT: 0/3/4.5 dB/oct; VQT/PBT/RTA: -3/0/1.5, 由插件按模式档值下发)。
  // 弹道在含斜率后的显示域运行: 高频的抬升进入"差距"本身, 回落时整体水平
  // 下沉, 不会出现高频吊尾; 表在斜率/模式/band 表变化时重建 (热路径只查表)。
  static constexpr float kSlopeRefHz = 632.45553f; // 支点 = 显示范围几何中心 sqrt(20·20000)
  float mSlopeDbPerOct = 0.f;   // 当前模式生效斜率 (dB/oct), 0 = 无倾斜
  std::vector<float> mSlopeDb;  // 每显示值斜率 (dB, FFT 256 band / 逐 band 引擎各 band)

  void RebuildSlopeGain() {
    std::vector<float> *freqs = nullptr;
    int n = 0;
    if (mMode == 0) {
      n = kSpectrumBands; // FFT: band 中心频率固定 (20..20k 对数均分, 见构造函数)
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
      mSlopeDb[b] = mSlopeDbPerOct * orm::FastLog2(f / kSlopeRefHz); // S·log2(f/f0) dB
    }
  }
  float mAttackCoeff = 0.2f;
  float mReleaseCoeff = 0.9f;
  int mReleaseMode = 0;      // 释放回落模式: 0=对数域(显示域差距等比收缩), 1=匀速(恒定屏高比例速率)
  float mAttackSec = 0.05f; // 上升时间常数 (s), 由插件下发 (固定 0.05s)
  float mReleaseSec = 0.2f; // 释放时间常数 (s), 由插件按速度档×释放模式派生下发
  float mBottomDb = -100.f; // 频谱显示下限 (dBFS), 由插件 Range 参数下发 (-80/-100/-120); 初始与参数默认一致
  int mNumBins = 2048;
  double mSampleRate = 48000.0;

  std::vector<Pt> mSpecPtsL;    // 预分配: L 填充点
  std::vector<Pt> mSpecPtsR;    // 预分配: R 填充点
  std::vector<Pt> mSpecPtsM;    // 预分配: 合并声道 (L+R) 填充点
  std::vector<Pt> mHoldPts;     // hold 曲线绘制点 (每帧重建, 与显示点同 x)

  // 频谱峰值保持 (与 mSpectrum 同域同长, 单位: 显示 dB, -1000 = 无有效峰值);
  // 开关/时长随电平表数据帧透传
  std::vector<float> mHoldSpec[3]; // 各通道 hold 显示 dB
  std::vector<float> mHoldAge[3];  // 各通道距上次刷新峰值的时间 (s)
  bool mHoldSignal = false;        // 已有有效峰值 (全 -1000 不画线)
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

  // ---- 方案0 绘制耗时测量 (快捷键 P 开关 HUD; F 开关 iPlug2 帧间耗时显示, 绑定见 Analyzer) ----
  static constexpr int kHudN = 120; // 滚动窗口: 最近 120 次 pad 绘制 (~2 s @60fps)
  float mHudMs[kHudN] = {};         // 每次 pad 绘制的 CPU 毫秒
  uint8_t mHudIsData[kHudN] = {};   // 该帧是否伴随数据包 (data/hover 帧分类)
  double mHudT[kHudN] = {};         // 采样时间戳 (steady_clock 纪元秒)
  int mHudPos = 0;
  int mHudFill = 0;
  uint32_t mDataPkts = 0; // 上次绘制以来收到的数据包计数 (帧分类用)
  bool mHudOn = false;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
