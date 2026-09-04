#pragma once

// OscilloscopeControl — 时域示波器控件 (触发模式 Trigger Mode)
//
// 特性：
// - 边沿触发 (Rising / Falling Edge) + 门限防抖迟滞 (Hysteresis) + 释抑 (Holdoff)
// - AUTO / NORM 两种工作模式
// - 时基选择 (1ms, 2ms, 5ms, 10ms, 20ms, 50ms)
// - 通道模式 (L/R 双轨重叠, L, R, M/S 中侧分解)
// - 垂直增益缩放 (1x, 2x, 4x)
// - 可交互拖拽门限线 (Trigger Level) 与零电平/削波参考网格
// - 鼠标悬停游标测量 (时间差 Δt、等效频率及音高、幅度)
// - 与全局 FREEZE、RESET 和主题系统无缝联动

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
  enum ETimebase {
    kTime1ms = 0,
    kTime2ms,
    kTime5ms,
    kTime10ms,
    kTime20ms,
    kTime50ms,
    kNumTimebases
  };

  enum EChanMode {
    kChanLR = 0,
    kChanL,
    kChanR,
    kChanMS,
    kNumChans
  };

  enum ETrigMode {
    kTrigAuto = 0,
    kTrigNorm,
    kNumTrigModes
  };

  enum ETrigEdge {
    kEdgeRising = 0,
    kEdgeFalling,
    kNumTrigEdges
  };

  enum ETrigSource {
    kSrcMid = 0,
    kSrcL,
    kSrcR,
    kNumTrigSources
  };

  enum ETrigState {
    kStateTriggered = 0,
    kStateAuto,
    kStateWait
  };

  explicit OscilloscopeControl(const IRECT &bounds) : IControl(bounds) {
    mDispL.reserve(4096);
    mDispR.reserve(4096);
    mDispM.reserve(4096);
    mDispS.reserve(4096);
  }

  void Clear() {
    mDispL.clear();
    mDispR.clear();
    mDispM.clear();
    mDispS.clear();
    mTrigState = kStateWait;
    SetDirty(false);
  }

  // 由插件 OnIdle() 每帧调用：从环形缓冲抽取触发波形切片
  void UpdateAudio(const float *const *pRing, int ringLen, int headPos, double sampleRate, bool frozen) {
    if (frozen && mFrozen)
      return;
    mFrozen = frozen;
    mSampleRate = std::max(1000.0, sampleRate);

    static constexpr double kTimeSecs[kNumTimebases] = {0.001, 0.002, 0.005, 0.010, 0.020, 0.050};
    const double winSec = kTimeSecs[mTimebaseIdx];
    const int nDisp = std::clamp((int)std::round(winSec * mSampleRate), 16, 16384);

    if (ringLen < nDisp + 64)
      return;

    // 选择触发源声道：0: Mid, 1: L, 2: R
    // 对应 pRing 索引：L=0, R=1, M=2
    const int trigChan = (mTrigSource == kSrcMid) ? 2 : (mTrigSource == kSrcL) ? 0 : 1;
    const float vTh = mTrigLevel;
    constexpr float kHys = 0.02f; // 门限防抖迟滞

    int trigIdx = -1;
    const int searchMax = std::min(ringLen - nDisp - 16, (int)(mSampleRate * 0.08));
    const int startP = (headPos - nDisp / 2 + ringLen) % ringLen;

    // 向后寻找满足边沿 + 迟滞条件的触发点
    for (int k = 1; k < searchMax; ++k) {
      const int p0 = (startP - k + ringLen) % ringLen;
      const int pPrev = (p0 - 1 + ringLen) % ringLen;
      const float s0 = pRing[trigChan][p0];
      const float sPrev = pRing[trigChan][pPrev];

      if (mTrigEdge == kEdgeRising) {
        if (sPrev <= (vTh - kHys) && s0 >= vTh) {
          trigIdx = p0;
          break;
        }
      } else {
        if (sPrev >= (vTh + kHys) && s0 <= vTh) {
          trigIdx = p0;
          break;
        }
      }
    }

    int readStart = 0;
    if (trigIdx >= 0) {
      mTrigState = kStateTriggered;
      readStart = (trigIdx - nDisp / 2 + ringLen) % ringLen;
    } else {
      if (mTrigMode == kTrigAuto) {
        mTrigState = kStateAuto;
        readStart = (headPos - nDisp + ringLen) % ringLen;
      } else {
        // NORM 模式：未触发时保留原有画面
        mTrigState = kStateWait;
        SetDirty(false);
        return;
      }
    }

    // 复制并准备绘制切片
    mDispL.resize(nDisp);
    mDispR.resize(nDisp);
    mDispM.resize(nDisp);
    mDispS.resize(nDisp);

    for (int i = 0; i < nDisp; ++i) {
      const int p = (readStart + i) % ringLen;
      const float l = pRing[0][p];
      const float r = pRing[1][p];
      mDispL[i] = l;
      mDispR[i] = r;
      mDispM[i] = (l + r) * 0.70710678f;
      mDispS[i] = (l - r) * 0.70710678f;
    }

    SetDirty(false);
  }

  void OnMouseDown(float x, float y, const IMouseMod &mod) override {
    // 检查是否点击顶部按钮
    if (y < mRECT.T + kHeaderH) {
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
        mZoomIdx = (mZoomIdx + 1) % 3;
        SetDirty(false);
        return;
      }
      if (mBtnMode.Contains(x, y)) {
        mTrigMode = (mTrigMode + 1) % kNumTrigModes;
        SetDirty(false);
        return;
      }
      if (mBtnEdge.Contains(x, y)) {
        mTrigEdge = (mTrigEdge + 1) % kNumTrigEdges;
        SetDirty(false);
        return;
      }
      if (mBtnSrc.Contains(x, y)) {
        mTrigSource = (mTrigSource + 1) % kNumTrigSources;
        SetDirty(false);
        return;
      }
    }

    // 检查是否点击触发门限手柄或波形区域进行拖拽
    const IRECT plot = GetPlotRect();
    if (plot.Contains(x, y)) {
      const float yTh = LevelToY(plot, mTrigLevel);
      if (std::fabs(y - yTh) < 14.f || x > plot.R - 28.f || mod.S || mod.C) {
        mDraggingTrig = true;
        mTrigLevel = YToLevel(plot, y);
        SetDirty(false);
        return;
      }
    }
  }

  void OnMouseDblClick(float x, float y, const IMouseMod &mod) override {
    const IRECT plot = GetPlotRect();
    if (plot.Contains(x, y)) {
      mTrigLevel = 0.0f;
      SetDirty(false);
    }
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod &mod) override {
    if (mDraggingTrig) {
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
    DrawTriggerLine(g, plot);
    DrawWaveforms(g, plot);
    DrawStatusBadges(g, plot);

    if (mHoverActive && plot.Contains(mHoverX, mHoverY) && !mDraggingTrig) {
      DrawCursorInspector(g, plot);
    }
  }

private:
  static constexpr float kHeaderH = 26.f;
  static constexpr float kZooms[3] = {1.0f, 2.0f, 4.0f};

  int mTimebaseIdx = kTime10ms;
  int mChanMode = kChanLR;
  int mTrigMode = kTrigAuto;
  int mTrigEdge = kEdgeRising;
  int mTrigSource = kSrcMid;
  int mZoomIdx = 0;
  float mTrigLevel = 0.0f;

  int mTrigState = kStateAuto;
  double mSampleRate = 48000.0;
  bool mFrozen = false;
  bool mDraggingTrig = false;

  bool mHoverActive = false;
  float mHoverX = 0.f;
  float mHoverY = 0.f;

  std::vector<float> mDispL;
  std::vector<float> mDispR;
  std::vector<float> mDispM;
  std::vector<float> mDispS;

  // 顶栏按钮布局区域
  IRECT mBtnChan, mBtnTime, mBtnZoom, mBtnMode, mBtnEdge, mBtnSrc;

  IRECT GetPlotRect() const {
    return IRECT(mRECT.L + 2.f, mRECT.T + kHeaderH + 2.f, mRECT.R - 2.f, mRECT.B - 2.f);
  }

  float GetCurrentZoom() const {
    return kZooms[std::clamp(mZoomIdx, 0, 2)];
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
    constexpr float topY = 3.f;
    const float y0 = mRECT.T + topY;
    const float y1 = y0 + btnH;
    constexpr float gap = 3.f;

    // 从右往左排布快捷按钮
    // 1. SRC (触发源)
    constexpr float wSrc = 52.f;
    mBtnSrc = IRECT(curR - wSrc, y0, curR, y1);
    curR -= (wSrc + gap);

    // 2. EDGE (触发沿: + / -)
    constexpr float wEdge = 20.f;
    mBtnEdge = IRECT(curR - wEdge, y0, curR, y1);
    curR -= (wEdge + gap);

    // 3. MODE (AUTO / NORM)
    constexpr float wMode = 44.f;
    mBtnMode = IRECT(curR - wMode, y0, curR, y1);
    curR -= (wMode + gap);

    // 4. ZOOM (1x / 2x / 4x)
    constexpr float wZoom = 26.f;
    mBtnZoom = IRECT(curR - wZoom, y0, curR, y1);
    curR -= (wZoom + gap);

    // 5. TIMEBASE (1ms..50ms)
    constexpr float wTime = 42.f;
    mBtnTime = IRECT(curR - wTime, y0, curR, y1);
    curR -= (wTime + gap);

    // 6. CHAN (L/R, L, R, M/S)
    constexpr float wChan = 36.f;
    mBtnChan = IRECT(curR - wChan, y0, curR, y1);
  }

  void DrawPill(IGraphics &g, const IRECT &box, const char *txt, bool active = false) {
    const bool hov = mHoverActive && box.Contains(mHoverX, mHoverY);
    const IColor bg = active ? COL_300() : COL_100();
    g.FillRect(bg, box);
    g.DrawRect(COL_300(), box);
    if (hov) {
      g.FillRect(HoverOverlay(), box);
    }
    const IText t(12, COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Middle);
    g.DrawText(t, txt, box);
  }

  void DrawHeader(IGraphics &g) {
    // 标题文本
    const IText titleT(14, COL_900(), kFontBold, EAlign::Near, EVAlign::Middle);
    const char *titleStr = (orm::UILang() == orm::kLangZH) ? "示波器" : "SCOPE";
    g.DrawText(titleT, titleStr, IRECT(mRECT.L + 6.f, mRECT.T + 3.f, mBtnChan.L - 4.f, mRECT.T + kHeaderH));

    // 按钮文案
    static const char *kChanNames[kNumChans] = {"L/R", "L", "R", "M/S"};
    DrawPill(g, mBtnChan, kChanNames[mChanMode]);

    static const char *kTimeNames[kNumTimebases] = {"1ms", "2ms", "5ms", "10ms", "20ms", "50ms"};
    DrawPill(g, mBtnTime, kTimeNames[mTimebaseIdx]);

    static const char *kZoomNames[3] = {"1x", "2x", "4x"};
    DrawPill(g, mBtnZoom, kZoomNames[mZoomIdx]);

    static const char *kModeNames[kNumTrigModes] = {"AUTO", "NORM"};
    DrawPill(g, mBtnMode, kModeNames[mTrigMode], mTrigMode == kTrigNorm);

    static const char *kEdgeNames[kNumTrigEdges] = {"+", "-"};
    DrawPill(g, mBtnEdge, kEdgeNames[mTrigEdge]);

    static const char *kSrcNames[kNumTrigSources] = {"SRC: M", "SRC: L", "SRC: R"};
    DrawPill(g, mBtnSrc, kSrcNames[mTrigSource]);
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
    const IColor limitCol = IColor(120, 226, 60, 52); // ±1.0 满幅度削波警戒线

    // 1. 水平刻度线
    // 零电平线 (Center 0.0)
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

    // 2. 垂直时间分度线 (4 等分)
    const float cx = plot.MW();
    for (int i = 1; i <= 3; ++i) {
      const float vx = plot.L + (float)i * 0.25f * plot.W();
      const bool isCenter = (i == 2);
      g.DrawLine(isCenter ? gridCol : subGridCol, vx, plot.T, vx, plot.B, nullptr, 1.f);
    }

    // 刻度微标 (+1.0 / 0.0 / -1.0)
    const IText tickT(10, COL_500(), kFontRegular, EAlign::Near, EVAlign::Middle);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%+.1f", 1.0f / zoom);
    g.DrawText(tickT, buf, IRECT(plot.L + 3.f, yPos10 - 6.f, plot.L + 32.f, yPos10 + 6.f));
    std::snprintf(buf, sizeof(buf), "%+.1f", -1.0f / zoom);
    g.DrawText(tickT, buf, IRECT(plot.L + 3.f, yNeg10 - 6.f, plot.L + 32.f, yNeg10 + 6.f));
  }

  void DrawTriggerLine(IGraphics &g, const IRECT &plot) {
    const float yTh = LevelToY(plot, mTrigLevel);
    if (yTh < plot.T || yTh > plot.B)
      return;

    const IColor trigCol = SemColor(MeterYellow());
    // 触发虚线 / 点线
    for (float x = plot.L; x < plot.R - 20.f; x += 8.f) {
      g.DrawLine(trigCol, x, yTh, std::min(x + 4.f, plot.R - 20.f), yTh, nullptr, 1.f);
    }

    // 右侧触发指示标记 Handle [T]
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

    // 当采样点多于像素宽度时使用 Min-Max 峰值抽取，保证高性能且无锯齿走样
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

        // 连结前后像素垂直列，消除断线
        const float drawTop = std::min(yTop, prevBot);
        const float drawBot = std::max(yBot, prevTop);
        g.DrawLine(col, x, drawTop, x, std::max(drawBot, drawTop + 1.f), nullptr, 1.f);

        prevTop = yTop;
        prevBot = yBot;
      }
    } else {
      // 采样点少于像素列时逐点平滑连线
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

      // 点数很少时（例如 <= 80 点）绘制微小采样点圆点
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
    // Side 通道颜色：明亮金色
    const IColor cS = IColor(255, 240, 185, 45);

    if (mChanMode == kChanLR) {
      // 双轨叠加：R 先画，L 后画
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
    }
  }

  void DrawStatusBadges(IGraphics &g, const IRECT &plot) {
    // 1. 左下角触发状态小徽标
    const float bY = plot.B - 18.f;
    IColor stateCol;
    const char *stateStr = "";
    if (mTrigState == kStateTriggered) {
      stateCol = SemColor(MeterGreen());
      stateStr = "TRIG'D";
    } else if (mTrigState == kStateAuto) {
      stateCol = SemColor(MeterYellow());
      stateStr = "AUTO";
    } else {
      stateCol = COL_500();
      stateStr = "WAIT";
    }

    const IRECT stateBadge(plot.L + 4.f, bY, plot.L + 46.f, bY + 14.f);
    g.FillRect(stateCol, stateBadge);
    const IText sT(10, ThemeMode() ? IColor(255, 20, 20, 20) : IColor(255, 255, 255, 255),
                   kFontBold, EAlign::Center, EVAlign::Middle);
    g.DrawText(sT, stateStr, stateBadge);

    // 门限值读数
    char lvlBuf[24];
    std::snprintf(lvlBuf, sizeof(lvlBuf), "Trig: %+.2f", mTrigLevel);
    const IText lvlT(11, COL_700(), kFontRegular, EAlign::Near, EVAlign::Middle);
    g.DrawText(lvlT, lvlBuf, IRECT(plot.L + 52.f, bY, plot.L + 130.f, bY + 14.f));

    // 2. 右下角时基与分度读数
    static constexpr double kTimeSecs[kNumTimebases] = {0.001, 0.002, 0.005, 0.010, 0.020, 0.050};
    const double winSec = kTimeSecs[mTimebaseIdx];
    char timeBuf[32];
    if (winSec < 0.001)
      std::snprintf(timeBuf, sizeof(timeBuf), "%.0f µs (%.0f µs/div)", winSec * 1e6, winSec * 0.25e6);
    else
      std::snprintf(timeBuf, sizeof(timeBuf), "%.1f ms (%.2f ms/div)", winSec * 1000.0, winSec * 250.0);

    const IText timeT(11, COL_700(), kFontRegular, EAlign::Far, EVAlign::Middle);
    g.DrawText(timeT, timeBuf, IRECT(plot.R - 160.f, bY, plot.R - 6.f, bY + 14.f));
  }

  void DrawCursorInspector(IGraphics &g, const IRECT &plot) {
    // 鼠标十字虚线
    g.DrawLine(COL_500(), mHoverX, plot.T, mHoverX, plot.B, nullptr, 1.f);
    g.DrawLine(COL_500(), plot.L, mHoverY, plot.R, mHoverY, nullptr, 1.f);

    // 计算相对于中心的 Δt 与电压值
    static constexpr double kTimeSecs[kNumTimebases] = {0.001, 0.002, 0.005, 0.010, 0.020, 0.050};
    const double winSec = kTimeSecs[mTimebaseIdx];
    const float dtSec = (float)((mHoverX - plot.MW()) / plot.W() * winSec);
    const float vVal = YToLevel(plot, mHoverY);

    char insBuf[64];
    const float dtMs = dtSec * 1000.f;
    if (std::fabs(dtSec) > 1e-5f) {
      const double fEquiv = 1.0 / std::fabs(dtSec);
      char note[12] = "";
      if (fEquiv >= 20.0 && fEquiv <= 20000.0) {
        FreqToNoteName(fEquiv, note, sizeof(note));
        std::snprintf(insBuf, sizeof(insBuf), "Δt: %+.2f ms  %.0f Hz (%s)  V: %+.2f", dtMs, fEquiv, note, vVal);
      } else {
        std::snprintf(insBuf, sizeof(insBuf), "Δt: %+.2f ms  V: %+.2f", dtMs, vVal);
      }
    } else {
      std::snprintf(insBuf, sizeof(insBuf), "Δt: 0.00 ms  V: %+.2f", vVal);
    }

    const IText t(11, COL_900(), kFontSemiBold, EAlign::Near, EVAlign::Middle);
    const float chipW = 210.f;
    const float chipX0 = std::clamp(mHoverX + 8.f, plot.L + 4.f, plot.R - chipW - 4.f);
    const float chipY0 = std::clamp(mHoverY - 20.f, plot.T + 4.f, plot.B - 22.f);
    const IRECT chip(chipX0, chipY0, chipX0 + chipW, chipY0 + 18.f);

    g.FillRect(COL_100(), chip);
    g.DrawRect(COL_500(), chip);
    g.DrawText(t, insBuf, chip.GetPadded(-4.f));
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
