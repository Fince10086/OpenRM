#pragma once

// OscilloscopeControl

#include "IControls.h"
#include "UiUtils.h"
#include "../Theme.h"
#include "../Strings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class OscilloscopeControl : public IControl {
public:
  enum ETriggerSource {
    kTrigEdge = 0,      // Rising Edge 门限触发
    kTrigAutocorr,      // Autocorrelation 自相关周期锁定
    kTrigFrequency,     // Frequency 定频定时刷新
    kNumTrigSources
  };

  enum EChanMode {
    kChanLR = 0,
    kChanL,
    kChanR,
    kChanMS,
    kChanSum,           // GRM 特色纯白全声道求和线
    kNumChans
  };

  enum ETimebase {
    kTime1ms = 0,
    kTime2ms,
    kTime5ms,
    kTime10ms,
    kTime20ms,
    kTime50ms,
    kTime100ms,
    kTime500ms,
    kTime1s,
    kTime2s,
    kNumTimebases
  };

  enum ETrigFreqPreset {
    kFreq10Hz = 0,
    kFreq20Hz,
    kFreq30Hz,
    kFreq60Hz,
    kFreq120Hz,
    kNumFreqPresets
  };

  enum ETrigChan {
    kSrcMid = 0,
    kSrcL,
    kSrcR,
    kNumTrigChans
  };

  explicit OscilloscopeControl(const IRECT &bounds) : IControl(bounds) {
    mDispL.reserve(8192);
    mDispR.reserve(8192);
    mDispM.reserve(8192);
    mDispS.reserve(8192);
    mDispSum.reserve(8192);
    mAcfCurve.assign(1024, 0.f);
  }

  void Clear() {
    mDispL.clear();
    mDispR.clear();
    mDispM.clear();
    mDispS.clear();
    mDispSum.clear();
    mAcfCurve.assign(1024, 0.f);
    mDetectedFreq = 0.f;
    mDetectedPeriodMs = 0.f;
    mDetectedLag = -1;
    mTrigStateActive = false;
    SetDirty(false);
  }

  // 插件 OnIdle() 每帧调用：根据当前模式执行触发并提取波形切片
  void UpdateAudio(const float *const *pRing, int ringLen, int headPos, double sampleRate, bool frozen) {
    if (frozen && mFrozen)
      return;
    mFrozen = frozen;
    mSampleRate = std::max(1000.0, sampleRate);

    static constexpr double kTimeSecs[kNumTimebases] = {
        0.001, 0.002, 0.005, 0.010, 0.020, 0.050, 0.100, 0.500, 1.000, 2.000};
    const double winSec = kTimeSecs[mTimebaseIdx];
    const int nDisp = std::clamp((int)std::round(winSec * mSampleRate), 16, ringLen - 64);

    if (ringLen < nDisp + 128)
      return;

    // 选择触发参考通道：L=0, R=1, Mid=2
    const int trigChan = (mTrigChan == kSrcMid) ? 2 : (mTrigChan == kSrcL) ? 0 : 1;

    int triggerPos = -1;
    mTrigStateActive = false;

    if (mTrigSource == kTrigEdge) {
      // 1. RISING EDGE 模式：
      const float vTh = mTrigLevel;
      constexpr float kHys = 0.02f;
      const int searchSpan = std::min(ringLen - nDisp - 16, (int)(mSampleRate * 0.15));

      for (int k = 1; k < searchSpan; ++k) {
        const int p0 = (headPos - k + ringLen) & (ringLen - 1);
        const int pPrev = (p0 - 1 + ringLen) & (ringLen - 1);
        const float s0 = pRing[trigChan][p0];
        const float sPrev = pRing[trigChan][pPrev];

        if (sPrev <= (vTh - kHys) && s0 >= vTh) {
          triggerPos = p0;
          mTrigStateActive = true;
          break;
        }
      }

      // 未搜寻到边沿时回退至以最新样本为右边缘
      if (triggerPos < 0)
        triggerPos = headPos;

    } else if (mTrigSource == kTrigAutocorr) {
      // 2. AUTOCORRELATION 模式：
      ComputeAutocorr(pRing[trigChan], ringLen, headPos);

      if (mDetectedLag > 8) {
        mTrigStateActive = true;
        const int searchRange = std::min(mDetectedLag * 2, ringLen - nDisp - 16);
        float bestVal = -1e9f;
        int bestIdx = headPos;
        for (int k = 0; k < searchRange; ++k) {
          const int p = (headPos - k + ringLen) & (ringLen - 1);
          if (pRing[trigChan][p] > bestVal) {
            bestVal = pRing[trigChan][p];
            bestIdx = p;
          }
        }
        triggerPos = bestIdx;
      } else {
        triggerPos = headPos;
      }

    } else {
      // 3. FREQUENCY 定频刷新模式：
      // 按照 Trigger frequency 定时抓取，右边缘为最新时刻
      static constexpr double kFreqValues[kNumFreqPresets] = {10.0, 20.0, 30.0, 60.0, 120.0};
      const double trigFreq = kFreqValues[mFreqPreset];
      const int stepSamples = std::max(1, (int)std::round(mSampleRate / trigFreq));

      // 累加采样步长进行节拍抽取
      const int advance = (headPos - mLastHeadPos + ringLen) & (ringLen - 1);
      mFreqAccum += advance;
      mLastHeadPos = headPos;

      if (mFreqAccum >= stepSamples || mDispL.empty()) {
        mFreqAccum %= stepSamples;
        triggerPos = headPos;
        mTrigStateActive = true;
      } else {
        // 未到刷新时钟周期时保持上一画面
        return;
      }
    }

    // 按照“右边缘为触发时刻”提取 nDisp 个前向历史采样点
    mDispL.resize(nDisp);
    mDispR.resize(nDisp);
    mDispM.resize(nDisp);
    mDispS.resize(nDisp);
    mDispSum.resize(nDisp);

    const int readStart = (triggerPos - nDisp + ringLen) & (ringLen - 1);
    for (int i = 0; i < nDisp; ++i) {
      const int p = (readStart + i) & (ringLen - 1);
      const float l = pRing[0][p];
      const float r = pRing[1][p];
      mDispL[i] = l;
      mDispR[i] = r;
      mDispM[i] = (l + r) * 0.70710678f;
      mDispS[i] = (l - r) * 0.70710678f;
      mDispSum[i] = (l + r); // GRM 全声道求和线
    }

    SetDirty(false);
  }

  void OnMouseDown(float x, float y, const IMouseMod &mod) override {
    // 1. 顶栏按钮交互
    if (y < mRECT.T + kHeaderH) {
      if (mBtnMode.Contains(x, y)) {
        mTrigSource = (mTrigSource + 1) % kNumTrigSources;
        SetDirty(false);
        return;
      }
      if (mBtnSecondary.Contains(x, y)) {
        if (mTrigSource == kTrigFrequency) {
          mFreqPreset = (mFreqPreset + 1) % kNumFreqPresets;
        } else {
          mTrigChan = (mTrigChan + 1) % kNumTrigChans;
        }
        SetDirty(false);
        return;
      }
      if (mBtnChan.Contains(x, y)) {
        mChanMode = (mChanMode + 1) % kNumChans;
        SetDirty(false);
        return;
      }
      if (mBtnTime.Contains(x, y)) {
        mTimebaseIdx = (mTimebaseIdx + 1) % kNumTimebases;
        SetDirty(false);
        return;
      }
      if (mBtnZoom.Contains(x, y)) {
        mZoomIdx = (mZoomIdx + 1) % 4;
        SetDirty(false);
        return;
      }
    }

    // 2. 波形区拖拽门限 (Rising edge 模式)
    const IRECT plot = GetPlotRect();
    if (plot.Contains(x, y)) {
      if (mTrigSource == kTrigEdge) {
        const float yTh = LevelToY(plot, mTrigLevel);
        if (std::fabs(y - yTh) < 14.f || x > plot.R - 32.f || mod.S || mod.C) {
          mDraggingTrig = true;
          mTrigLevel = YToLevel(plot, y);
          SetDirty(false);
          return;
        }
      }
    }
  }

  void OnMouseDblClick(float x, float y, const IMouseMod &mod) override {
    const IRECT plot = GetPlotRect();
    if (plot.Contains(x, y)) {
      // 官方交互：“Double-click to reset zoom”
      mZoomIdx = 0;       // 复位缩放 1x
      mTrigLevel = 0.0f;  // 复位门限 0.0
      SetDirty(false);
    }
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod &mod) override {
    if (mDraggingTrig && mTrigSource == kTrigEdge) {
      const IRECT plot = GetPlotRect();
      mTrigLevel = YToLevel(plot, y);
      SetDirty(false);
    }
  }

  void OnMouseUp(float x, float y, const IMouseMod &mod) override {
    mDraggingTrig = false;
  }

  void OnMouseOver(float x, float y, const IMouseMod &mod) override {
    mHoverX = x;
    mHoverY = y;
    mHoverActive = true;
    SetDirty(false);
  }

  void OnMouseOut() override {
    mHoverActive = false;
    mDraggingTrig = false;
    SetDirty(false);
  }

  void Draw(IGraphics &g) override {
    g.FillRect(COL_100(), mRECT);

    UpdatePillLayout();
    DrawHeader(g);

    const IRECT plot = GetPlotRect();
    DrawPlotBackground(g, plot);
    DrawGrid(g, plot);

    // 在 Autocorrelation 模式下叠印自相关灰色曲线与周期标尺线
    if (mTrigSource == kTrigAutocorr) {
      DrawAutocorrOverlay(g, plot);
    }

    // 在 Rising Edge 模式下绘制可拖拽门限虚线与手柄
    if (mTrigSource == kTrigEdge) {
      DrawTriggerLine(g, plot);
    }

    DrawWaveforms(g, plot);
    DrawStatusBadges(g, plot);

    if (mHoverActive && plot.Contains(mHoverX, mHoverY) && !mDraggingTrig) {
      DrawCursorInspector(g, plot);
    }
  }

private:
  static constexpr float kHeaderH = 26.f;
  static constexpr float kZooms[4] = {1.0f, 2.0f, 4.0f, 8.0f};

  int mTrigSource = kTrigEdge;
  int mChanMode = kChanLR;
  int mTimebaseIdx = kTime10ms;
  int mZoomIdx = 0;
  int mFreqPreset = kFreq30Hz;
  int mTrigChan = kSrcMid;
  float mTrigLevel = 0.0f;

  double mSampleRate = 48000.0;
  bool mFrozen = false;
  bool mDraggingTrig = false;
  bool mTrigStateActive = false;

  int mLastHeadPos = 0;
  int mFreqAccum = 0;

  // 自相关算法检测结果与归一化曲线
  std::vector<float> mAcfCurve;
  float mDetectedFreq = 0.f;
  float mDetectedPeriodMs = 0.f;
  int mDetectedLag = -1;

  bool mHoverActive = false;
  float mHoverX = 0.f;
  float mHoverY = 0.f;

  std::vector<float> mDispL;
  std::vector<float> mDispR;
  std::vector<float> mDispM;
  std::vector<float> mDispS;
  std::vector<float> mDispSum;

  // 顶栏按钮布局区域
  IRECT mBtnMode, mBtnSecondary, mBtnChan, mBtnTime, mBtnZoom;

  IRECT GetPlotRect() const {
    return IRECT(mRECT.L + 2.f, mRECT.T + kHeaderH + 2.f, mRECT.R - 2.f, mRECT.B - 2.f);
  }

  float GetCurrentZoom() const {
    return kZooms[std::clamp(mZoomIdx, 0, 3)];
  }

  float LevelToY(const IRECT &plot, float lvl) const {
    const float cy = plot.MH();
    const float halfH = plot.H() * 0.48f;
    return cy - lvl * GetCurrentZoom() * halfH;
  }

  float YToLevel(const IRECT &plot, float y) const {
    const float cy = plot.MH();
    const float halfH = plot.H() * 0.48f;
    const float raw = (cy - y) / (halfH * GetCurrentZoom());
    return std::clamp(raw, -1.0f, 1.0f);
  }

  void UpdatePillLayout() {
    float curR = mRECT.R - 4.f;
    constexpr float btnH = 20.f;
    const float y0 = mRECT.T + 3.f;
    const float y1 = y0 + btnH;
    constexpr float gap = 3.f;

    // 从右往左排布快捷按钮
    // 1. ZOOM (1x / 2x / 4x / 8x)
    constexpr float wZoom = 26.f;
    mBtnZoom = IRECT(curR - wZoom, y0, curR, y1);
    curR -= (wZoom + gap);

    // 2. TIMEBASE (1ms..2s)
    constexpr float wTime = 42.f;
    mBtnTime = IRECT(curR - wTime, y0, curR, y1);
    curR -= (wTime + gap);

    // 3. CHAN (L/R, L, R, M/S, SUM)
    constexpr float wChan = 38.f;
    mBtnChan = IRECT(curR - wChan, y0, curR, y1);
    curR -= (wChan + gap);

    // 4. SECONDARY (SRC: L/R/M 或 FREQ: 30Hz)
    const float wSec = (mTrigSource == kTrigFrequency) ? 46.f : 50.f;
    mBtnSecondary = IRECT(curR - wSec, y0, curR, y1);
    curR -= (wSec + gap);

    // 5. MODE (EDGE / AUTOCORR / FREQ)
    constexpr float wMode = 68.f;
    mBtnMode = IRECT(curR - wMode, y0, curR, y1);
  }

  void DrawPill(IGraphics &g, const IRECT &box, const char *txt, bool active = false) {
    const bool hov = mHoverActive && box.Contains(mHoverX, mHoverY);
    const IColor bg = active ? COL_300() : COL_100();
    g.FillRect(bg, box);
    g.DrawRect(COL_300(), box);
    if (hov) {
      g.FillRect(HoverOverlay(), box);
    }
    const IText t(11, COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Middle);
    g.DrawText(t, txt, box);
  }

  void DrawHeader(IGraphics &g) {
    const IText titleT(14, COL_900(), kFontBold, EAlign::Near, EVAlign::Middle);
    const char *titleStr = (orm::UILang() == orm::kLangZH) ? "示波器" : "SCOPE";
    g.DrawText(titleT, titleStr, IRECT(mRECT.L + 6.f, mRECT.T + 3.f, mBtnMode.L - 4.f, mRECT.T + kHeaderH));

    // 1. 触发模式按钮
    static const char *kModeNames[kNumTrigSources] = {"EDGE", "AUTOCORR", "FREQ"};
    DrawPill(g, mBtnMode, kModeNames[mTrigSource], true);

    // 2. 二级控制项
    if (mTrigSource == kTrigFrequency) {
      static const char *kFreqNames[kNumFreqPresets] = {"10Hz", "20Hz", "30Hz", "60Hz", "120Hz"};
      DrawPill(g, mBtnSecondary, kFreqNames[mFreqPreset]);
    } else {
      static const char *kSrcNames[kNumTrigChans] = {"SRC: M", "SRC: L", "SRC: R"};
      DrawPill(g, mBtnSecondary, kSrcNames[mTrigChan]);
    }

    // 3. 通道模式按钮 (包含 SUM)
    static const char *kChanNames[kNumChans] = {"L/R", "L", "R", "M/S", "SUM"};
    DrawPill(g, mBtnChan, kChanNames[mChanMode]);

    // 4. 时基
    static const char *kTimeNames[kNumTimebases] = {
        "1ms", "2ms", "5ms", "10ms", "20ms", "50ms", "100ms", "500ms", "1s", "2s"};
    DrawPill(g, mBtnTime, kTimeNames[mTimebaseIdx]);

    // 5. 垂直缩放
    static const char *kZoomNames[4] = {"1x", "2x", "4x", "8x"};
    DrawPill(g, mBtnZoom, kZoomNames[mZoomIdx]);
  }

  void DrawPlotBackground(IGraphics &g, const IRECT &plot) {
    g.FillRect(COL_100(), plot);
    g.DrawRect(COL_300(), plot);
  }

  void DrawGrid(IGraphics &g, const IRECT &plot) {
    const float cy = plot.MH();
    const float zoom = GetCurrentZoom();
    const IColor gridCol = WarmGray(ThemeMode() ? 40 : 220);
    const IColor subGridCol = WarmGray(ThemeMode() ? 25 : 235);
    const IColor limitCol = IColor(120, 226, 60, 52); // ±1.0 满幅度警戒线

    // 零电平中轴线
    g.DrawLine(gridCol, plot.L, cy, plot.R, cy, nullptr, 1.f);

    // ±0.5 线
    const float yPos05 = LevelToY(plot, 0.5f);
    const float yNeg05 = LevelToY(plot, -0.5f);
    if (yPos05 > plot.T && yPos05 < plot.B)
      g.DrawLine(subGridCol, plot.L, yPos05, plot.R, yPos05, nullptr, 1.f);
    if (yNeg05 > plot.T && yNeg05 < plot.B)
      g.DrawLine(subGridCol, plot.L, yNeg05, plot.R, yNeg05, nullptr, 1.f);

    // ±1.0 极限线 (0 dBFS)
    const float yPos10 = LevelToY(plot, 1.0f);
    const float yNeg10 = LevelToY(plot, -1.0f);
    if (yPos10 >= plot.T)
      g.DrawLine(limitCol, plot.L, yPos10, plot.R, yPos10, nullptr, 1.f);
    if (yNeg10 <= plot.B)
      g.DrawLine(limitCol, plot.L, yNeg10, plot.R, yNeg10, nullptr, 1.f);

    // 垂直等分线
    for (int i = 1; i <= 3; ++i) {
      const float vx = plot.L + (float)i * 0.25f * plot.W();
      g.DrawLine(subGridCol, vx, plot.T, vx, plot.B, nullptr, 1.f);
    }

    // 右侧触发时刻边界线 (GRM 规范：最右侧是触发事件发生时刻)
    const IColor eventCol = (mTrigSource == kTrigEdge) ? SemColor(MeterYellow()) : gridCol;
    g.DrawLine(eventCol, plot.R, plot.T, plot.R, plot.B, nullptr, 1.5f);

    // 幅度刻度微标
    const IText tickT(10, COL_500(), kFontRegular, EAlign::Near, EVAlign::Middle);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%+.1f", 1.0f / zoom);
    g.DrawText(tickT, buf, IRECT(plot.L + 3.f, yPos10 - 6.f, plot.L + 32.f, yPos10 + 6.f));
    std::snprintf(buf, sizeof(buf), "%+.1f", -1.0f / zoom);
    g.DrawText(tickT, buf, IRECT(plot.L + 3.f, yNeg10 - 6.f, plot.L + 32.f, yNeg10 + 6.f));
  }

  // 绘制 Autocorrelation 模式专有的叠加信息：灰色 ACF 曲线与垂直周期点线
  void DrawAutocorrOverlay(IGraphics &g, const IRECT &plot) {
    if (mAcfCurve.empty())
      return;

    const int nAcf = (int)mAcfCurve.size();
    const float cy = plot.MH();
    const float halfH = plot.H() * 0.45f;
    const float w = plot.W();

    // 1. 灰色自相关波形曲线 (grey line)
    const IColor acfCol = ThemeMode() ? IColor(110, 160, 160, 160) : IColor(110, 90, 90, 90);
    g.PathClear();
    for (int px = 0; px < (int)w; ++px) {
      const float frac = (float)px / w;
      const int idx = std::clamp((int)(frac * (float)nAcf), 0, nAcf - 1);
      const float y = cy - mAcfCurve[idx] * halfH;
      if (px == 0)
        g.PathMoveTo(plot.L + (float)px, y);
      else
        g.PathLineTo(plot.L + (float)px, y);
    }
    g.PathStroke(IPattern(acfCol), 1.2f);

    // 2. 垂直点状指示线：标注最可信周期 (vertical dotted bar indicates most probable period)
    if (mDetectedLag > 0 && mDetectedLag < nAcf) {
      const float peakX = plot.L + ((float)mDetectedLag / (float)nAcf) * w;
      if (peakX >= plot.L && peakX <= plot.R) {
        const IColor barCol = SemColor(MeterGreen());
        for (float y = plot.T + 2.f; y < plot.B - 2.f; y += 6.f) {
          g.DrawLine(barCol, peakX, y, peakX, std::min(y + 3.f, plot.B - 2.f), nullptr, 1.5f);
        }
      }
    }
  }

  // 绘制 Rising Edge 模式下的可拖动水平门限线
  void DrawTriggerLine(IGraphics &g, const IRECT &plot) {
    const float yTh = LevelToY(plot, mTrigLevel);
    if (yTh < plot.T || yTh > plot.B)
      return;

    const IColor trigCol = SemColor(MeterYellow());
    for (float x = plot.L; x < plot.R - 18.f; x += 8.f) {
      g.DrawLine(trigCol, x, yTh, std::min(x + 4.f, plot.R - 18.f), yTh, nullptr, 1.f);
    }

    // 右边缘触发手柄 [T]
    const IRECT handle(plot.R - 18.f, yTh - 7.f, plot.R - 2.f, yTh + 7.f);
    g.FillRect(trigCol, handle);
    const IText t(10, ThemeMode() ? IColor(255, 20, 20, 20) : IColor(255, 255, 255, 255),
                  kFontBold, EAlign::Center, EVAlign::Middle);
    g.DrawText(t, "T", handle);
  }

  void DrawWaveformLine(IGraphics &g, const IRECT &plot, const std::vector<float> &wave, const IColor &col) {
    const int n = (int)wave.size();
    if (n < 2)
      return;

    const float zoom = GetCurrentZoom();
    const float cy = plot.MH();
    const float halfH = plot.H() * 0.48f;
    const float w = plot.W();

    if (n > (int)w) {
      const float step = (float)n / w;
      float prevTop = cy, prevBot = cy;

      for (int px = 0; px < (int)w; ++px) {
        const int i0 = (int)(px * step);
        const int i1 = std::min(n, (int)((px + 1) * step));
        float minVal = wave[i0];
        float maxVal = wave[i0];
        for (int i = i0 + 1; i < i1; ++i) {
          minVal = std::min(minVal, wave[i]);
          maxVal = std::max(maxVal, wave[i]);
        }

        const float yTop = cy - std::clamp(maxVal * zoom, -1.15f, 1.15f) * halfH;
        const float yBot = cy - std::clamp(minVal * zoom, -1.15f, 1.15f) * halfH;
        const float x = plot.L + (float)px;

        const float drawTop = std::min(yTop, prevBot);
        const float drawBot = std::max(yBot, prevTop);
        g.DrawLine(col, x, drawTop, x, std::max(drawBot, drawTop + 1.f), nullptr, 1.2f);

        prevTop = yTop;
        prevBot = yBot;
      }
    } else {
      g.PathClear();
      for (int i = 0; i < n; ++i) {
        const float frac = (float)i / (float)(n - 1);
        const float x = plot.L + frac * w;
        const float y = cy - std::clamp(wave[i] * zoom, -1.15f, 1.15f) * halfH;
        if (i == 0)
          g.PathMoveTo(x, y);
        else
          g.PathLineTo(x, y);
      }
      g.PathStroke(IPattern(col), 1.5f);

      if (n <= 80) {
        for (int i = 0; i < n; ++i) {
          const float frac = (float)i / (float)(n - 1);
          const float x = plot.L + frac * w;
          const float y = cy - std::clamp(wave[i] * zoom, -1.15f, 1.15f) * halfH;
          g.FillCircle(col, x, y, 1.5f);
        }
      }
    }
  }

  void DrawWaveforms(IGraphics &g, const IRECT &plot) {
    IColor cL, cR, cM;
    GetChannelColors(cL, cR, cM);
    const IColor cS = IColor(255, 240, 185, 45);
    // GRM 全声道求和线：明亮纯白
    const IColor cSum = ThemeMode() ? IColor(255, 255, 255, 255) : IColor(255, 30, 30, 30);

    if (mChanMode == kChanLR) {
      if (!mDispR.empty())
        DrawWaveformLine(g, plot, mDispR, IColor(210, cR.R, cR.G, cR.B));
      if (!mDispL.empty())
        DrawWaveformLine(g, plot, mDispL, IColor(240, cL.R, cL.G, cL.B));
    } else if (mChanMode == kChanL) {
      if (!mDispL.empty())
        DrawWaveformLine(g, plot, mDispL, cL);
    } else if (mChanMode == kChanR) {
      if (!mDispR.empty())
        DrawWaveformLine(g, plot, mDispR, cR);
    } else if (mChanMode == kChanMS) {
      if (!mDispS.empty())
        DrawWaveformLine(g, plot, mDispS, IColor(200, cS.R, cS.G, cS.B));
      if (!mDispM.empty())
        DrawWaveformLine(g, plot, mDispM, IColor(240, cM.R, cM.G, cM.B));
    } else if (mChanMode == kChanSum) {
      // 纯白求和线
      if (!mDispSum.empty())
        DrawWaveformLine(g, plot, mDispSum, cSum);
    }
  }

  void DrawStatusBadges(IGraphics &g, const IRECT &plot) {
    const float bY = plot.B - 18.f;

    // 1. 左侧状态徽标
    IColor stateCol;
    const char *stateStr = "";
    if (mTrigSource == kTrigEdge) {
      stateCol = mTrigStateActive ? SemColor(MeterGreen()) : COL_500();
      stateStr = mTrigStateActive ? "TRIG'D" : "WAIT";
    } else if (mTrigSource == kTrigAutocorr) {
      stateCol = (mDetectedLag > 0) ? SemColor(MeterGreen()) : COL_500();
      stateStr = (mDetectedLag > 0) ? "LOCKED" : "NOISE";
    } else {
      stateCol = SemColor(MeterYellow());
      stateStr = "STROBE";
    }

    const IRECT stateBadge(plot.L + 4.f, bY, plot.L + 46.f, bY + 14.f);
    g.FillRect(stateCol, stateBadge);
    const IText sT(10, ThemeMode() ? IColor(255, 20, 20, 20) : IColor(255, 255, 255, 255),
                   kFontBold, EAlign::Center, EVAlign::Middle);
    g.DrawText(sT, stateStr, stateBadge);

    // 2. 模式专有读数
    char infoBuf[48] = "";
    if (mTrigSource == kTrigEdge) {
      std::snprintf(infoBuf, sizeof(infoBuf), "Trig: %+.2f", mTrigLevel);
    } else if (mTrigSource == kTrigAutocorr) {
      if (mDetectedFreq > 0.f)
        std::snprintf(infoBuf, sizeof(infoBuf), "Det: %.1f Hz (%.2f ms)", mDetectedFreq, mDetectedPeriodMs);
      else
        std::snprintf(infoBuf, sizeof(infoBuf), "%s", "Det: —");
    } else {
      static constexpr double kFreqValues[kNumFreqPresets] = {10.0, 20.0, 30.0, 60.0, 120.0};
      std::snprintf(infoBuf, sizeof(infoBuf), "Rate: %.0f Hz", kFreqValues[mFreqPreset]);
    }

    const IText infoT(11, COL_700(), kFontRegular, EAlign::Near, EVAlign::Middle);
    g.DrawText(infoT, infoBuf, IRECT(plot.L + 52.f, bY, plot.L + 180.f, bY + 14.f));

    // 3. 右下角时基读数
    static constexpr double kTimeSecs[kNumTimebases] = {
        0.001, 0.002, 0.005, 0.010, 0.020, 0.050, 0.100, 0.500, 1.000, 2.000};
    const double winSec = kTimeSecs[mTimebaseIdx];
    char timeBuf[32];
    if (winSec < 0.010)
      std::snprintf(timeBuf, sizeof(timeBuf), "%.1f ms (%.2f ms/div)", winSec * 1000.0, winSec * 250.0);
    else if (winSec < 1.0)
      std::snprintf(timeBuf, sizeof(timeBuf), "%.0f ms (%.1f ms/div)", winSec * 1000.0, winSec * 250.0);
    else
      std::snprintf(timeBuf, sizeof(timeBuf), "%.1f s (%.2f s/div)", winSec, winSec * 0.25);

    const IText timeT(11, COL_700(), kFontRegular, EAlign::Far, EVAlign::Middle);
    g.DrawText(timeT, timeBuf, IRECT(plot.R - 160.f, bY, plot.R - 6.f, bY + 14.f));
  }

  void DrawCursorInspector(IGraphics &g, const IRECT &plot) {
    g.DrawLine(COL_500(), mHoverX, plot.T, mHoverX, plot.B, nullptr, 1.f);
    g.DrawLine(COL_500(), plot.L, mHoverY, plot.R, mHoverY, nullptr, 1.f);

    static constexpr double kTimeSecs[kNumTimebases] = {
        0.001, 0.002, 0.005, 0.010, 0.020, 0.050, 0.100, 0.500, 1.000, 2.000};
    const double winSec = kTimeSecs[mTimebaseIdx];
    // GRM 坐标：最右侧是 t = 0，向左为负时间 (-dt)
    const float dtSec = (float)((mHoverX - plot.R) / plot.W() * winSec);
    const float vVal = YToLevel(plot, mHoverY);

    char insBuf[80];
    const float dtMs = dtSec * 1000.f;
    if (mTrigSource == kTrigAutocorr && mDetectedFreq > 0.f) {
      char note[12] = "";
      FreqToNoteName(mDetectedFreq, note, sizeof(note));
      std::snprintf(insBuf, sizeof(insBuf), "t: %+.1f ms  V: %+.2f | Det: %.1f Hz (%s)",
                    dtMs, vVal, mDetectedFreq, note);
    } else if (std::fabs(dtSec) > 1e-5f) {
      const double fEquiv = 1.0 / std::fabs(dtSec);
      char note[12] = "";
      if (fEquiv >= 20.0 && fEquiv <= 20000.0) {
        FreqToNoteName(fEquiv, note, sizeof(note));
        std::snprintf(insBuf, sizeof(insBuf), "t: %+.1f ms  V: %+.2f | %.0f Hz (%s)", dtMs, vVal, fEquiv, note);
      } else {
        std::snprintf(insBuf, sizeof(insBuf), "t: %+.1f ms  V: %+.2f", dtMs, vVal);
      }
    } else {
      std::snprintf(insBuf, sizeof(insBuf), "t: 0.0 ms  V: %+.2f", vVal);
    }

    const IText t(11, COL_900(), kFontSemiBold, EAlign::Near, EVAlign::Middle);
    const float chipW = 230.f;
    const float chipX0 = std::clamp(mHoverX + 8.f, plot.L + 4.f, plot.R - chipW - 4.f);
    const float chipY0 = std::clamp(mHoverY - 20.f, plot.T + 4.f, plot.B - 22.f);
    const IRECT chip(chipX0, chipY0, chipX0 + chipW, chipY0 + 18.f);

    g.FillRect(COL_100(), chip);
    g.DrawRect(COL_500(), chip);
    g.DrawText(t, insBuf, chip.GetPadded(-4.f));
  }

  // 利用基-2 FFT 计算自相关函数并提取最可信周期
  void ComputeAutocorr(const float *srcRing, int ringLen, int headPos) {
    constexpr int N = 1024;
    constexpr int FFT_N = 2048; // 补零消除循环卷积伪影

    std::array<float, FFT_N> re{}, im{};
    float mean = 0.f;

    // 拷贝 N 个样本并计算均值
    for (int i = 0; i < N; ++i) {
      const int p = (headPos - N + i + ringLen) & (ringLen - 1);
      re[i] = srcRing[p];
      mean += re[i];
    }
    mean /= (float)N;
    for (int i = 0; i < N; ++i)
      re[i] -= mean;

    // 1. 正向 FFT
    FftRadix2(re.data(), im.data(), FFT_N);

    // 2. 功率谱
    for (int i = 0; i < FFT_N; ++i) {
      re[i] = re[i] * re[i] + im[i] * im[i];
      im[i] = 0.f;
    }

    // 3. 逆 FFT (实偶对称序列的 IFFT 等价于再次正向 FFT)
    FftRadix2(re.data(), im.data(), FFT_N);

    const float energy0 = re[0];
    if (energy0 < 1e-8f) {
      mAcfCurve.assign(N, 0.f);
      mDetectedFreq = 0.f;
      mDetectedPeriodMs = 0.f;
      mDetectedLag = -1;
      return;
    }

    // 归一化自相关曲线
    mAcfCurve.resize(N);
    const float invE = 1.0f / energy0;
    for (int tau = 0; tau < N; ++tau) {
      mAcfCurve[tau] = std::clamp(re[tau] * invE, -1.0f, 1.0f);
    }

    // 4. 寻找最可信周期峰 (Most probable period)
    const int minLag = std::max(10, (int)std::round(mSampleRate / 2500.0)); // 2500 Hz 上限
    const int maxLag = std::min(N - 4, (int)std::round(mSampleRate / 25.0));   // 25 Hz 下限

    // 跨过主瓣下降区
    int valleyLag = 1;
    while (valleyLag < maxLag && mAcfCurve[valleyLag] > 0.3f && mAcfCurve[valleyLag] <= mAcfCurve[valleyLag - 1]) {
      ++valleyLag;
    }

    int bestLag = -1;
    float bestPeak = 0.25f; // 最低自相关置信门限

    for (int lag = std::max(minLag, valleyLag); lag < maxLag; ++lag) {
      if (mAcfCurve[lag] > bestPeak &&
          mAcfCurve[lag] > mAcfCurve[lag - 1] &&
          mAcfCurve[lag] >= mAcfCurve[lag + 1]) {
        bestPeak = mAcfCurve[lag];
        bestLag = lag;
      }
    }

    if (bestLag > 0) {
      // 二次抛物线亚采样插值
      const float y0 = mAcfCurve[bestLag - 1];
      const float y1 = mAcfCurve[bestLag];
      const float y2 = mAcfCurve[bestLag + 1];
      const float denom = 2.0f * (y0 - 2.0f * y1 + y2);
      const float delta = (std::fabs(denom) > 1e-6f) ? (y0 - y2) / denom : 0.f;
      const float exactLag = (float)bestLag + std::clamp(delta, -0.5f, 0.5f);

      mDetectedLag = (int)std::round(exactLag);
      mDetectedPeriodMs = (exactLag / (float)mSampleRate) * 1000.f;
      mDetectedFreq = (float)mSampleRate / exactLag;
    } else {
      mDetectedLag = -1;
      mDetectedFreq = 0.f;
      mDetectedPeriodMs = 0.f;
    }
  }

  static void FftRadix2(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; ++i) {
      int bit = n >> 1;
      for (; j & bit; bit >>= 1)
        j ^= bit;
      j ^= bit;
      if (i < j) {
        std::swap(re[i], re[j]);
        std::swap(im[i], im[j]);
      }
    }
    for (int len = 2; len <= n; len <<= 1) {
      const float ang = -2.f * (float)PI / (float)len;
      const float wRe = std::cos(ang), wIm = std::sin(ang);
      const int half = len >> 1;
      for (int i = 0; i < n; i += len) {
        float cRe = 1.f, cIm = 0.f;
        for (int k = 0; k < half; ++k) {
          const float uRe = re[i + k], uIm = im[i + k];
          const float vRe = re[i + k + half] * cRe - im[i + k + half] * cIm;
          const float vIm = re[i + k + half] * cIm + im[i + k + half] * cRe;
          re[i + k] = uRe + vRe;
          im[i + k] = uIm + vIm;
          re[i + k + half] = uRe - vRe;
          im[i + k + half] = uIm - vIm;
          const float nRe = cRe * wRe - cIm * wIm;
          cIm = cRe * wIm + cIm * wRe;
          cRe = nRe;
        }
      }
    }
  }

  static void FreqToNoteName(double hz, char *out, int outSize) {
    static const char *const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int nn = (int)std::lround(12.0 * std::log2(std::max(hz, 1.0) / 440.0)) + 69;
    std::snprintf(out, outSize, "%s%d", kNoteNames[(nn % 12 + 12) % 12], nn / 12 - 1);
  }

  static float MeterSatScale() {
    const int sat = ThemeSatMax();
    return (sat <= 30) ? (float)sat / 30.f : (1.f + (float)(sat - 30) / 55.f);
  }

  static IColor SemColor(IColor c) {
    const float m = std::clamp(MeterSatScale(), 0.f, 1.f);
    const float lum = 0.299f * c.R + 0.587f * c.G + 0.114f * c.B;
    return IColor(c.A, (int)std::lround(c.R * m + lum * (1.f - m)),
                        (int)std::lround(c.G * m + lum * (1.f - m)),
                        (int)std::lround(c.B * m + lum * (1.f - m)));
  }
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
