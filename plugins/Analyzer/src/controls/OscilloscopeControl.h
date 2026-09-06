#pragma once

// OscilloscopeControl

#include "IControls.h"
#include "UiUtils.h"
#include "../Theme.h"

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
    kTrigRoll = 0,      // ROLL 实时连续窗口：无触发，右边缘为最新采样点
    kTrigSync,          // SYNC 同步锁相：自相关基频周期锁定
    kTrigSweep,         // SWEEP 自由扫描：笔随样本前进，写满时间窗回卷覆盖旧迹线（老式示波器）
    kNumTrigSources
  };

  // 声道显示掩码：bit0=L, bit1=R, bit2=M，可任意组合叠加
  enum EChanBit {
    kChanBitL = 1,
    kChanBitR = 2,
    kChanBitM = 4,
  };

  // 默认显示值：时间窗 100 ms (0.100s)，幅度 2x 缩放（顶部刻度 +0.5）
  static constexpr double kDefaultWindowSec = 0.100;
  static constexpr float kDefaultZoom = 2.0f;

  explicit OscilloscopeControl(const IRECT &bounds) : IControl(bounds) {
    mDispL.reserve(8192);
    mDispR.reserve(8192);
    mDispM.reserve(8192);
    mDispS.reserve(8192);
    mDispSum.reserve(8192);
  }

  void Clear() {
    mDispL.clear();
    mDispR.clear();
    mDispM.clear();
    mDispS.clear();
    mDispSum.clear();
    mDetectedFreq = 0.f;
    mDetectedPeriodMs = 0.f;
    mDetectedLag = -1;
    mTrigStateActive = false;
    mSweepPos = 0;
    mFrozenAnchor = -1;
    mExtractedWinSec = -1.0;
    mAbsHead = 0;
    mPrevHeadPos = -1;
    mDispRightAbs = -1;
    SetDirty(false);
  }

  // 顶栏按钮行（Analyzer.cpp 布局）直接驱动的纯 UI 状态
  int GetTrigSource() const { return mTrigSource; }
  int GetChanMask() const { return mChanMask; }
  double GetWindowSec() const { return mWindowSec; }
  float GetZoomFactor() const { return mZoomFactor; }
  void SetTrigSource(int v) {
    mTrigSource = std::clamp(v, 0, kNumTrigSources - 1);
    mSweepPos = 0; // 切模式后从左缘开始覆盖旧画面
    SetDirty(false);
  }
  void SetChanMask(int v) { mChanMask = std::clamp(v, 0, kChanBitL | kChanBitR | kChanBitM); SetDirty(false); }
  void SetWindowSec(double s) { mWindowSec = std::clamp(s, kMinWindowSec, kMaxWindowSec); SetDirty(false); }
  void SetZoomFactor(float z) { mZoomFactor = std::clamp(z, 1.f, kMaxZoom); SetDirty(false); }

  // 内嵌滑块的布局矩形（Analyzer.cpp 布局用），画区为其让位
  // 缩放滑块在左缘：条左缘贴控件边、右缘贴画区 (无缝)，纵向上下与画区齐平
  static constexpr float kSliderW = 20.f;
  static constexpr float kSliderH = 20.f;
  IRECT GetZoomSliderRect() const {
    return IRECT(mRECT.L, mRECT.T + 2.f, mRECT.L + kSliderW, mRECT.B - kSliderH - 6.f);
  }
  IRECT GetTimeSliderRect() const {
    return IRECT(mRECT.L + kSliderW, mRECT.B - kSliderH - 2.f, mRECT.R - 2.f, mRECT.B - 2.f);
  }

  // 触发参考通道随显示声道自动决定：M 开启（或 L+R 同显）用 M，仅 L/R 单显用该声道
  int EffectiveTrigChan() const {
    if (mChanMask & kChanBitM)
      return 2;
    if ((mChanMask & kChanBitL) && !(mChanMask & kChanBitR))
      return 0;
    if ((mChanMask & kChanBitR) && !(mChanMask & kChanBitL))
      return 1;
    return 2;
  }

  // 插件 OnIdle() 每帧调用：根据当前模式执行触发并提取波形切片
  void UpdateAudio(const float *const *pRing, int ringLen, int headPos, double sampleRate, bool frozen) {
    mSampleRate = std::max(1000.0, sampleRate);

    const double winSec = mWindowSec;
    const int nDisp = std::clamp((int)std::round(winSec * mSampleRate), 16, ringLen - 64);

    if (ringLen < nDisp + 128)
      return;

    if (frozen && mFrozen) {
      if (mFrozenAnchor >= 0 && winSec != mExtractedWinSec) {
        ExtractWindow(pRing, ringLen, mFrozenAnchor, nDisp);
        mDispRightAbs = mAbsHead - 1 - ((headPos - mFrozenAnchor + ringLen) & (ringLen - 1));
        mSweepPos = 0; // SWEEP 解冻后扫描笔从左缘重写
        mExtractedWinSec = winSec;
        SetDirty(false);
      }
      return;
    }
    mFrozen = frozen;

    // 绝对样本计数: 环位置回卷也能累计单调递增的头部绝对号 (数据锁定分箱用)
    if (mPrevHeadPos >= 0) {
      const int adv = (headPos - mPrevHeadPos + ringLen) & (ringLen - 1);
      mAbsHead += adv;
    }
    mPrevHeadPos = headPos;

    // 触发参考通道随显示声道掩码自动决定（L=0, R=1, Mid=2）
    const int trigChan = EffectiveTrigChan();

    int triggerPos = -1;
    mTrigStateActive = false;

    if (mTrigSource == kTrigRoll) {
      // 1. ROLL 模式：
      // 无需边沿搜索，右边缘始终为最新音频采样点（实时连续观察窗）
      triggerPos = headPos;
      mTrigStateActive = true;

    } else if (mTrigSource == kTrigSync) {
      // 2. SYNC 模式（自相关基波周期同步锁相）：
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
      // 3. SWEEP 模式：自由扫描（sMexoscope 式）
      // 笔随音频样本连续前进，逐样本写入显示缓冲；写满一个时间窗后回卷左缘，
      // 继续覆盖旧迹线（右侧未覆盖部分保持可见），扫描一轮时间 = 窗口时长本身。
      // 时间窗变化时清空缓冲、笔归零，重新扫满一屏。
      mDispRightAbs = -1; // 扫描笔缓冲非连续时间窗, 包络退回像素锁定分箱
      if ((int)mDispL.size() != nDisp) {
        mDispL.assign(nDisp, 0.f);
        mDispR.assign(nDisp, 0.f);
        mDispM.assign(nDisp, 0.f);
        mDispS.assign(nDisp, 0.f);
        mDispSum.assign(nDisp, 0.f);
        mSweepPos = 0;
      }

      if (mLastHeadPos < 0) {
        mLastHeadPos = headPos; // 首帧只记录位置，避免假差值
        return;
      }
      const int advance = (headPos - mLastHeadPos + ringLen) & (ringLen - 1);
      mLastHeadPos = headPos;
      if (advance <= 0)
        return;
      // 环长度远大于窗口，正常 advance 不会超过窗口长度；超长（极端卡顿）截断
      const int nAdvance = std::min(advance, nDisp);

      for (int k = nAdvance; k > 0; --k) { // 从最旧到最新逐样本落笔
        const int p = (headPos - k + ringLen) & (ringLen - 1);
        const float l = pRing[0][p];
        const float r = pRing[1][p];
        mDispL[mSweepPos] = l;
        mDispR[mSweepPos] = r;
        mDispM[mSweepPos] = (l + r) * 0.70710678f;
        mDispS[mSweepPos] = (l - r) * 0.70710678f;
        mDispSum[mSweepPos] = (l + r);
        if (++mSweepPos >= nDisp)
          mSweepPos = 0;
      }
      mTrigStateActive = true;
      if (frozen) { // 冻结瞬间: 锚定扫描笔最新落点 (环内最新样本)
        mFrozenAnchor = (headPos - 1 + ringLen) & (ringLen - 1);
        mExtractedWinSec = winSec;
      }
      SetDirty(false);
      return;
    }

    // 冻结瞬间: 锚定本次触发位置, 冻结期间拖时间窗据此按新窗长重取迹线
    if (frozen)
      mFrozenAnchor = triggerPos;

    ExtractWindow(pRing, ringLen, triggerPos, nDisp);
    mDispRightAbs = mAbsHead - 1 - ((headPos - triggerPos + ringLen) & (ringLen - 1));
    mExtractedWinSec = winSec;
    SetDirty(false);
  }

  // 以 rightEdge 为右缘 (触发时刻), 从环形缓冲提取 nDisp 点前向历史到显示缓冲
  void ExtractWindow(const float *const *pRing, int ringLen, int rightEdge, int nDisp) {
    mDispL.resize(nDisp);
    mDispR.resize(nDisp);
    mDispM.resize(nDisp);
    mDispS.resize(nDisp);
    mDispSum.resize(nDisp);

    const int readStart = (rightEdge - nDisp + ringLen) & (ringLen - 1);
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
  }

  void OnMouseDown(float x, float y, const IMouseMod &mod) override {
  }

  void OnMouseDblClick(float x, float y, const IMouseMod &mod) override {
    const IRECT plot = GetPlotRect();
    if (plot.Contains(x, y)) {
      // 双击复位至默认缩放 2x (+0.5)
      mZoomFactor = kDefaultZoom;
      SetDirty(false);
    }
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod &mod) override {
  }

  void OnMouseUp(float x, float y, const IMouseMod &mod) override {
  }

  void OnMouseOver(float x, float y, const IMouseMod &mod) override {
    mHoverX = x;
    mHoverY = y;
    mHoverActive = true;
    SetDirty(false);
  }

  void OnMouseOut() override {
    mHoverActive = false;
    SetDirty(false);
  }

  void Draw(IGraphics &g) override {
    g.FillRect(COL_100(), mRECT);

    const IRECT plot = GetPlotRect();
    DrawPlotBackground(g, plot);

    // hover 标签占位先行计算, 供静态刻度避让 (同频谱 skipRect 机制)
    const bool hov = mHoverActive && plot.Contains(mHoverX, mHoverY);
    const IRECT detR = (mTrigSource == kTrigSync) ? DetRect(plot) : IRECT();
    IRECT hovTime, hovAmp;
    if (hov) {
      ComputeHover(g, plot, detR);
      hovTime = mHovTimeR.Union(mHovFreqR);
      hovAmp = mHovAmpR;
    }

    DrawWaveforms(g, plot);
    DrawAxis(g, plot, hovTime, hovAmp, detR);
    DrawStatusReadout(g, plot);

    if (hov)
      DrawHover(g, plot);
  }

private:
  // 时间窗与幅度倍率的连续范围（内嵌滑块驱动）
  static constexpr double kMinWindowSec = 0.010;
  static constexpr double kMaxWindowSec = 2.000;
  static constexpr float kMaxZoom = 8.f;
  // 贴边标签几何 (与频谱图刻度排版一致)
  static constexpr float kTickRight = 3.f;
  static constexpr float kLabelH = 16.f;
  // 时间列 1-2-5 阶梯 (ms 滞后)
  static constexpr double kTimeCells[] = {0.1,  0.2,  0.5,   1.0,   2.0,   5.0,  10.0,
                                          20.0, 50.0, 100.0, 200.0, 500.0, 1000.0};
  // 中间档: 每个主档 ×1.5 (50→75→100), 用于劈开过宽的色块档
  static constexpr double kTimeCellsMid[] = {0.15, 0.3,  0.75, 1.5,   3.0,   7.5,   15.0,
                                             30.0, 75.0, 150.0, 300.0, 750.0, 1500.0};
  // 幅度阶梯: ±1.0 (0 dBFS) 起逐级减半 (-6 dB 步进), 升序存放 (与时间表一致)
  static constexpr double kAmpLadder[] = {0.0078125, 0.015625, 0.03125, 0.0625, 0.125, 0.25, 0.5, 1.0};
  // 中间档: 每档间隙的几何中点 (主档 ×√½, 即 +3 dB; -6→-9→-12), 升序
  static constexpr double kAmpLadderMid[] = {0.00552427, 0.01104854, 0.02209709, 0.04419417,
                                             0.08838835, 0.1767767,  0.35355339, 0.70710678};
  static constexpr int kNumTimeCells = (int)(sizeof(kTimeCells) / sizeof(kTimeCells[0]));
  static constexpr int kNumAmpSteps = (int)(sizeof(kAmpLadder) / sizeof(kAmpLadder[0]));

  int mTrigSource = kTrigSync; // 默认 SYNC 模式
  int mChanMask = kChanBitM; // 默认显示 M
  double mWindowSec = kDefaultWindowSec;
  float mZoomFactor = kDefaultZoom;
  int mSweepPos = 0; // FREQ 扫描笔位置（显示缓冲槽位）

  double mSampleRate = 48000.0;
  bool mFrozen = false;
  bool mTrigStateActive = false;

  int mLastHeadPos = -1;
  // 绝对样本计数 (跨环回卷单调递增) 与显示窗右缘样本的绝对号:
  // 抽稀包络按绝对号锁定分箱边界, 包络随数据亚像素平滑平移, 消除长时基下的逐帧重相位抖动
  long long mAbsHead = 0;
  int mPrevHeadPos = -1;
  long long mDispRightAbs = -1;
  // 冻结锚点: 冻结瞬间迹线右缘的环内位置; 冻结中拖时间窗据此按新窗长重取迹线
  int mFrozenAnchor = -1;
  // 迹线当前对应的窗长 (秒); 冻结中用于检测时间窗是否被拖动
  double mExtractedWinSec = -1.0;

  // 自相关算法检测结果
  float mDetectedFreq = 0.f;
  float mDetectedPeriodMs = 0.f;
  int mDetectedLag = -1;

  // 自相关 scratch 缓冲（4096 点窗 / 8192 点 FFT，移到成员避免每帧栈上 64KB）
  std::array<float, 8192> mFftRe{};
  std::array<float, 8192> mFftIm{};
  std::array<float, 4096> mAcfBuf{};

  bool mHoverActive = false;
  float mHoverX = 0.f;
  float mHoverY = 0.f;

  // hover 贴边读数 (顶缘时间/频率 + 右缘幅度)
  char mHovTimeBuf[16] = "";
  char mHovFreqBuf[24] = "";
  char mHovAmpBuf[8] = "";
  IRECT mHovTimeR, mHovFreqR, mHovAmpR;

  std::vector<float> mDispL;
  std::vector<float> mDispR;
  std::vector<float> mDispM;
  std::vector<float> mDispS;
  std::vector<float> mDispSum;

  // 抽稀包络的复用缓冲（高时基 min/max 填充带渲染）
  std::vector<float> mEnvTop;
  std::vector<float> mEnvBot;
  std::vector<float> mEnvX;

  IRECT GetPlotRect() const {
    // 左缘 = 缩放滑块条右缘 (无缝), 下缘 = 时间滑块条上缘 (无缝), 右/上各留 2px
    return IRECT(mRECT.L + kSliderW, mRECT.T + 2.f, mRECT.R - 2.f, mRECT.B - kSliderH - 2.f);
  }

  float GetCurrentZoom() const {
    return std::clamp(mZoomFactor, 1.f, kMaxZoom);
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

  // 色块背景: 横 5 块 × 纵每半边 5 带平色块。取值域对齐声像图色块的实际合成区间
  // (约 184-237: 其角向三档 230/215/195 与径向渐变 245→172 按 0.5·(角+径) 合成,
  //  极端端点从不成对出现) —— 直接沿用端点值会合成出 245 的格子, 比声像图
  //  浅得更浅、深得更深。
  // 分界取加密阶梯 (每个主档间隙补一颗中间档: 时间 ×1.5 / 幅度 +3dB) 中量程内的
  // 最高 4 颗 —— 原 1-2-5 阶梯在窗长/量程贴近上一档主刻度时会拼出 50-60% 的整块
  // (如 200→500ms、zoom=1 时纵向两带各半); 加密后任一档不超过画面约 40%。
  // 中间档只作色块分界, 数字标签仅最外一颗 (见 DrawAxis)。
  // 横向越靠右缘 (当前时刻) 越亮; 纵向越靠中轴越亮 (同声像图角向的方向),
  // 给中轴处的深色波形让出对比
  void DrawPlotBackground(IGraphics &g, const IRECT &plot) {
    const float zoom = GetCurrentZoom();
    const float halfH = plot.H() * 0.48f;
    const float cy = plot.MH();
    const float visMax = (plot.H() * 0.5f) / (halfH * zoom);
    const double winMs = mWindowSec * 1000.0;

    // 纵向带界: 加密幅度阶梯里低于量程顶的最高 4 颗 + 中轴
    constexpr float kVCol[5] = {176.f, 190.f, 204.f, 218.f, 232.f}; // 外→内: 中心最亮, 衬托深色波形
    double ampB[4];
    bool ampMaj[4];
    LargestLadder(visMax, kAmpLadder, kNumAmpSteps, kAmpLadderMid, kNumAmpSteps, 4, ampB, ampMaj);
    float bnds[6];
    int nB = 0;
    bnds[nB++] = visMax;
    for (int i = 0; i < 4 && ampB[i] > 0.0; ++i)
      bnds[nB++] = (float)ampB[i];
    bnds[nB++] = 0.f;

    // 横向列界: 窗缘 + 加密时间阶梯里窗内最大 4 颗 (右→左; 刻度降序返回, 须自最小档排起)
    double tickB[4];
    bool tickMaj[4];
    LargestLadder(winMs, kTimeCells, kNumTimeCells, kTimeCellsMid, kNumTimeCells, 4, tickB, tickMaj);
    float xs[6];
    int nCols = 0;
    xs[nCols++] = plot.R;
    for (int i = 3; i >= 0; --i) {
      if (tickB[i] < 0)
        continue;
      const float x = plot.R - (float)(tickB[i] / winMs) * plot.W();
      if (x - plot.L < 1.5f)
        continue;
      xs[nCols++] = x;
    }
    xs[nCols++] = plot.L;
    constexpr float kHCol[5] = {240.f, 228.f, 216.f, 204.f, 192.f}; // 右→左 (越新越亮)

    for (int r = 0; r + 1 < nB; ++r) {
      const float yT0 = cy - bnds[r] * zoom * halfH, yT1 = cy - bnds[r + 1] * zoom * halfH;
      const float yB0 = cy + bnds[r + 1] * zoom * halfH, yB1 = cy + bnds[r] * zoom * halfH;
      for (int c = 0; c + 1 < nCols; ++c) {
        const IColor cell = WarmGray((int)std::lround(0.5f * (kHCol[c] + kVCol[r])));
        g.FillRect(cell, IRECT(xs[c + 1], yT0, xs[c], yT1));
        g.FillRect(cell, IRECT(xs[c + 1], yB0, xs[c], yB1));
      }
    }
  }

  // 合并主档/中间档两张升序阶梯表, 取量程内最大 n 档 (降序); 不足 n 档时末位以 -1 填充。
  // 两张表都必须升序 (首元素最小) —— 时间/幅度四表已统一为升序存放。
  // major 标记主档 (1-2-5 / 逐级减半); 中间档只作色块分界, 数字标签仅最外一颗
  static void LargestLadder(double scale, const double *major, int nMajor, const double *mid, int nMid,
                            int n, double *outV, bool *outMaj) {
    int iM = nMajor - 1, iI = nMid - 1, found = 0;
    while (found < n && (iM >= 0 || iI >= 0)) {
      const bool takeMajor = iM >= 0 && (iI < 0 || major[iM] > mid[iI]);
      const double v = takeMajor ? major[iM] : mid[iI];
      if (takeMajor)
        --iM;
      else
        --iI;
      if (v < scale * (1.0 - 1e-4)) {
        outV[found] = v;
        outMaj[found] = takeMajor;
        ++found;
      }
    }
    for (int i = found; i < n; ++i) {
      outV[i] = -1.0;
      outMaj[i] = false;
    }
  }

  // 中轴语义线 + 贴边刻度: 幅度右缘, 时间顶缘 (同频谱)
  void DrawAxis(IGraphics &g, const IRECT &plot, const IRECT &hovTime, const IRECT &hovAmp,
                const IRECT &detR) {
    const float zoom = GetCurrentZoom();
    const float halfH = plot.H() * 0.48f;
    const float visMax = (plot.H() * 0.5f) / (halfH * zoom);
    const double winMs = mWindowSec * 1000.0;

    // 零电平中轴线
    g.DrawLine(COL_500(), plot.L, plot.MH(), plot.R, plot.MH(), nullptr, 1.f);

    // 幅度标签: 上半幅主档阶梯值 (0/-6/-12 dBFS), 右缘线上方; 与下一档间距 ≥18px 才标;
    // 避让按实测文本矩形 —— 固定 52px 的框远宽于实际文字, 按框判交会让时间
    // 标签在真正重叠前就提前消失; 收集实测矩形供时间标签避让。
    // 中间档仅在它是量程内最外一颗分界时补标 (量程落在 -6~-9 之间的 "-9" 等)
    const IText ampT(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Bottom);
    IRECT ampRs[6];
    int nAmpR = 0;
    for (int i = kNumAmpSteps - 1; i >= 0; --i) {
      const double t = kAmpLadder[i];
      if (t >= visMax * (1.f - 1e-4f))
        continue;
      if ((t - 0.5 * t) * zoom * halfH < 18.f)
        break;
      const float y = LevelToY(plot, (float)t);
      const IRECT labelR = (y - kLabelH - 1.f >= plot.T)
                               ? IRECT(plot.R - 52.f, y - kLabelH - 1.f, plot.R - kTickRight, y - 1.f)
                               : IRECT(plot.R - 52.f, y + 1.f, plot.R - kTickRight, y + 1.f + kLabelH);
      char buf[8];
      std::snprintf(buf, sizeof(buf), "%d", (int)std::lround(20.0 * std::log10(t)));
      IRECT fitR = labelR;
      g.MeasureText(ampT, buf, fitR);
      if ((!hovAmp.Empty() && fitR.Intersects(hovAmp)) ||
          (!hovTime.Empty() && fitR.Intersects(hovTime)) || nAmpR >= 6)
        continue;
      g.DrawText(ampT, buf, labelR);
      ampRs[nAmpR++] = fitR;
    }

    double ampB[4];
    bool ampMaj[4];
    LargestLadder(visMax, kAmpLadder, kNumAmpSteps, kAmpLadderMid, kNumAmpSteps, 4, ampB, ampMaj);
    if (!ampMaj[0] && ampB[0] > 0.0) {
      double m1 = 0.0;
      for (int i = kNumAmpSteps - 1; i >= 0; --i) {
        if (kAmpLadder[i] < visMax * (1.f - 1e-4f)) {
          m1 = kAmpLadder[i];
          break;
        }
      }
      if ((ampB[0] - m1) * zoom * halfH >= 18.f) {
        const float y = LevelToY(plot, (float)ampB[0]);
        const IRECT labelR = (y - kLabelH - 1.f >= plot.T)
                                 ? IRECT(plot.R - 52.f, y - kLabelH - 1.f, plot.R - kTickRight, y - 1.f)
                                 : IRECT(plot.R - 52.f, y + 1.f, plot.R - kTickRight, y + 1.f + kLabelH);
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", (int)std::lround(20.0 * std::log10(ampB[0])));
        IRECT fitR = labelR;
        g.MeasureText(ampT, buf, fitR);
        bool blocked = (!hovAmp.Empty() && fitR.Intersects(hovAmp)) ||
                       (!hovTime.Empty() && fitR.Intersects(hovTime));
        for (int ai = 0; ai < nAmpR && !blocked; ++ai)
          blocked = fitR.Intersects(ampRs[ai]);
        if (!blocked && nAmpR < 6) {
          g.DrawText(ampT, buf, labelR);
          ampRs[nAmpR++] = fitR;
        }
      }
    }

    // 时间标签: 顶缘线右侧, 加密阶梯窗内前两颗 —— 主档必标, 中间档仅当最外一颗时标
    // (窗长在 75~100ms 之间时标 "-75ms"; 超过 100ms 后回落为 "-100ms"/"-50ms");
    // 避让 hover 读数、SYNC 检测读数与幅度标签
    const IText timeT(14, COL_700(), kFontRegular, EAlign::Near, EVAlign::Top);
    double tickB[4];
    bool tickMaj[4];
    LargestLadder(winMs, kTimeCells, kNumTimeCells, kTimeCellsMid, kNumTimeCells, 4, tickB, tickMaj);
    int nTimeDrawn = 0;
    for (int i = 0; i < 4 && nTimeDrawn < 2; ++i) {
      if (tickB[i] < 0)
        break;
      if (!tickMaj[i] && i > 0)
        continue;
      const float x = plot.R - (float)(tickB[i] / winMs) * plot.W();
      char buf[12];
      if (tickB[i] < 1000.0)
        std::snprintf(buf, sizeof(buf), "-%gms", tickB[i]);
      else
        std::snprintf(buf, sizeof(buf), "-%.1fs", tickB[i] * 0.001);
      const IRECT labelR(x + 5.f, plot.T + 2.f, plot.R, plot.T + 2.f + kLabelH);
      IRECT fit = labelR;
      g.MeasureText(timeT, buf, fit);
      bool blocked = (!hovTime.Empty() && fit.Intersects(hovTime)) ||
                     (!detR.Empty() && fit.Intersects(detR));
      for (int ai = 0; ai < nAmpR && !blocked; ++ai)
        blocked = fit.Intersects(ampRs[ai]);
      if (blocked)
        continue;
      g.DrawText(timeT, buf, labelR);
      ++nTimeDrawn;
    }
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
      // 点数超过像素列数时抽稀为每列 min/max 包络，画成上下折线闭合的填充带：
      // 相邻列间走抗锯齿斜面，避免逐列硬边竖条在高时基下的像素感。
      // 分箱边界锚定绝对样本号 (随数据一同平移) 而非像素列：长时基下每帧内容平移量
      // 不是列宽整数倍，像素锁定分箱各列 min/max 的相位逐帧漂移，包络边缘会轻微抖动；
      // 数据锁定后旧样本恒在同一箱内，包络形状稳定，整体随内容亚像素平滑左移。
      // SWEEP 扫描笔缓冲非连续时间窗 (mDispRightAbs < 0)，phase=0 退化为像素锁定分箱。
      const int cols = std::max(2, (int)w);
      const double stepD = (double)n / (double)cols;
      double phase = 0.0;
      if (mDispRightAbs >= 0) {
        phase = std::fmod((double)(n - 1) - (double)mDispRightAbs, stepD);
        if (phase < 0.0)
          phase += stepD;
      }
      mEnvTop.clear();
      mEnvBot.clear();
      mEnvX.clear();
      double bPrev = 0.0;
      double b = (phase > 0.0) ? phase : stepD;
      while (bPrev < (double)n - 0.5) {
        const int i0 = std::max(0, (int)std::ceil(bPrev));
        const int i1 = std::min(n, (int)std::ceil(b));
        if (i1 > i0) {
          float minVal = wave[i0];
          float maxVal = wave[i0];
          for (int i = i0 + 1; i < i1; ++i) {
            minVal = std::min(minVal, wave[i]);
            maxVal = std::max(maxVal, wave[i]);
          }
          mEnvTop.push_back(cy - std::clamp(maxVal * zoom, -1.15f, 1.15f) * halfH);
          mEnvBot.push_back(cy - std::clamp(minVal * zoom, -1.15f, 1.15f) * halfH);
          mEnvX.push_back(plot.L + (float)((bPrev + b) * 0.5 / (double)n) * w); // 箱中心 → 亚像素 x
        }
        bPrev = b;
        b += stepD;
      }

      const int nb = (int)mEnvX.size();
      g.PathClear();
      for (int px = 0; px < nb; ++px) {
        if (px == 0)
          g.PathMoveTo(mEnvX[0], mEnvTop[0]);
        else
          g.PathLineTo(mEnvX[px], mEnvTop[px]);
      }
      for (int px = nb - 1; px >= 0; --px)
        g.PathLineTo(mEnvX[px], mEnvBot[px]);
      g.PathClose();
      g.PathFill(IPattern(col));
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

    const int mask = std::clamp(mChanMask, 0, kChanBitL | kChanBitR | kChanBitM);
    const int nSel = ((mask & kChanBitL) ? 1 : 0) + ((mask & kChanBitR) ? 1 : 0) + ((mask & kChanBitM) ? 1 : 0);
    if (!nSel)
      return;

    // 多选叠加时降低不透明度（绘制顺序 R → L → M），单选全亮
    const int alpha = (nSel > 1) ? 210 : 255;
    if (mask & kChanBitR)
      DrawWaveformLine(g, plot, mDispR, IColor(alpha, cR.R, cR.G, cR.B));
    if (mask & kChanBitL)
      DrawWaveformLine(g, plot, mDispL, IColor(alpha, cL.R, cL.G, cL.B));
    if (mask & kChanBitM)
      DrawWaveformLine(g, plot, mDispM, IColor(alpha, cM.R, cM.G, cM.B));
  }

  // SYNC 基频检测读数: 底缘右下, 贴边纯文本 (同频谱刻度排版)
  IRECT DetRect(const IRECT &plot) const {
    return IRECT(plot.R - 190.f, plot.B - 2.f - kLabelH, plot.R - kTickRight, plot.B - 2.f);
  }

  void DrawStatusReadout(IGraphics &g, const IRECT &plot) {
    if (mTrigSource != kTrigSync)
      return;

    char infoBuf[48];
    if (mDetectedFreq > 0.f)
      std::snprintf(infoBuf, sizeof(infoBuf), "Det: %.1f Hz (%.2f ms)", mDetectedFreq, mDetectedPeriodMs);
    else
      std::snprintf(infoBuf, sizeof(infoBuf), "Det: —");
    g.DrawText(IText(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Top), infoBuf, DetRect(plot));
  }

  // hover 贴边读数占位: 时间(+等效频率音名)贴顶缘、幅度贴右缘, 与频谱 hover 同款
  void ComputeHover(IGraphics &g, const IRECT &plot, const IRECT &detR) {
    const double winMs = mWindowSec * 1000.0;
    const float dtMs = (float)((mHoverX - plot.R) / plot.W() * winMs);
    if (std::fabs(dtMs) >= 1000.f)
      std::snprintf(mHovTimeBuf, sizeof(mHovTimeBuf), "%+.2f s", dtMs * 0.001f);
    else
      std::snprintf(mHovTimeBuf, sizeof(mHovTimeBuf), "%+.1f ms", dtMs);

    mHovFreqBuf[0] = '\0';
    if (std::fabs(dtMs) > 0.01f) {
      const double fEq = 1000.0 / std::fabs((double)dtMs);
      if (fEq >= 20.0 && fEq <= 20000.0) {
        char note[12];
        FreqToNoteName(fEq, note, sizeof(note));
        std::snprintf(mHovFreqBuf, sizeof(mHovFreqBuf), "%.0f Hz (%s)", fEq, note);
      }
    }
    std::snprintf(mHovAmpBuf, sizeof(mHovAmpBuf), "%+.2f", YToLevel(plot, mHoverY));

    // 顶缘: 时间标签在指针左, 等效频率在指针右, 放不下时依次换边
    const IText t(14, COL_700(), kFontRegular, EAlign::Near, EVAlign::Top);
    IRECT rT(mHoverX, plot.T + 2.f, mHoverX, plot.T + 2.f + kLabelH), rF = rT;
    g.MeasureText(t, mHovTimeBuf, rT);
    g.MeasureText(t, mHovFreqBuf, rF);
    const float wT = rT.W(), wF = rF.W();
    constexpr float kGap = 5.f;
    const bool tFitsL = mHoverX - kGap - wT >= plot.L;
    const bool fFitsR = wF > 0.f && mHoverX + kGap + wF <= plot.R;
    if (tFitsL && fFitsR) {
      rT = IRECT(mHoverX - kGap - wT, plot.T + 2.f, mHoverX - kGap, plot.T + 2.f + kLabelH);
      rF = IRECT(mHoverX + kGap, plot.T + 2.f, mHoverX + kGap + wF, plot.T + 2.f + kLabelH);
    } else if (!tFitsL) {
      rT = IRECT(mHoverX + kGap, plot.T + 2.f, mHoverX + kGap + wT, plot.T + 2.f + kLabelH);
      rF = IRECT(rT.R + 2.f, plot.T + 2.f, rT.R + 2.f + wF, plot.T + 2.f + kLabelH);
    } else {
      rT = IRECT(mHoverX - kGap - wT, plot.T + 2.f, mHoverX - kGap, plot.T + 2.f + kLabelH);
      rF = IRECT(rT.L - 2.f - wF, plot.T + 2.f, rT.L - 2.f, plot.T + 2.f + kLabelH);
    }
    // SYNC 检测读数占位时整体下移让开
    if (!detR.Empty() && (rT.Intersects(detR) || rF.Intersects(detR))) {
      const float dy = detR.H() + 2.f;
      rT.T += dy;
      rT.B += dy;
      rF.T += dy;
      rF.B += dy;
    }
    mHovTimeR = rT;
    mHovFreqR = rF;

    // 右缘: 幅度读数贴线上方, 贴近顶缘或撞上 SYNC 读数时翻到线下; 避让用实测矩形
    if (mHoverY - kLabelH - 1.f >= plot.T)
      mHovAmpR = IRECT(plot.R - 52.f, mHoverY - kLabelH - 1.f, plot.R - kTickRight, mHoverY - 1.f);
    else
      mHovAmpR = IRECT(plot.R - 52.f, mHoverY + 1.f, plot.R - kTickRight, mHoverY + 1.f + kLabelH);
    if (!detR.Empty() && mHovAmpR.Intersects(detR))
      mHovAmpR = IRECT(plot.R - 52.f, mHoverY + 1.f, plot.R - kTickRight, mHoverY + 1.f + kLabelH);
    g.MeasureText(IText(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Bottom), mHovAmpBuf,
                  mHovAmpR);
  }

  void DrawHover(IGraphics &g, const IRECT &plot) {
    g.DrawLine(COL_700(), mHoverX, plot.T, mHoverX, plot.B, nullptr, 1.f);
    g.DrawLine(COL_700(), plot.L, mHoverY, plot.R, mHoverY, nullptr, 1.f);
    g.DrawText(IText(14, COL_700(), kFontRegular, EAlign::Near, EVAlign::Top), mHovTimeBuf, mHovTimeR);
    if (mHovFreqBuf[0])
      g.DrawText(IText(14, COL_700(), kFontRegular, EAlign::Near, EVAlign::Top), mHovFreqBuf, mHovFreqR);
    g.DrawText(IText(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Bottom), mHovAmpBuf, mHovAmpR);
  }

  // 利用基-2 FFT 计算自相关函数并提取最可信周期。
  // 归一化采用无偏补偿: FFT 线性相关在延迟 τ 处只有 N−τ 个重叠样本, 直接除以
  // r[0] 会隐含 (N−τ)/N 的三角偏置, 长周期峰被系统性压制 (two tone 318 Hz 会
  // 锁到 2.7 ms 的次级峰而非 12.5 ms 的合成重复周期)。乘 N/(N−τ) 补偿后, 大延
  // 迟处方差被放大, 故搜索上限再压到 0.85·N 限制重叠不低于 15%。
  void ComputeAutocorr(const float *srcRing, int ringLen, int headPos) {
    constexpr int N = 4096;
    constexpr int FFT_N = 8192; // 补零消除循环卷积伪影

    float *re = mFftRe.data();
    float *im = mFftIm.data();
    std::fill(re, re + FFT_N, 0.f);
    std::fill(im, im + FFT_N, 0.f);
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
    FftRadix2(re, im, FFT_N);

    // 2. 功率谱
    for (int i = 0; i < FFT_N; ++i) {
      re[i] = re[i] * re[i] + im[i] * im[i];
      im[i] = 0.f;
    }

    // 3. 逆 FFT (实偶对称序列的 IFFT 等价于再次正向 FFT)
    FftRadix2(re, im, FFT_N);

    const float energy0 = re[0];
    if (energy0 < 1e-8f) {
      mDetectedFreq = 0.f;
      mDetectedPeriodMs = 0.f;
      mDetectedLag = -1;
      return;
    }

    // 4. 归一化自相关曲线（仅用于峰值搜索，不再绘制）
    // 延迟上限只受 0.85·N 约束 (48 kHz ≈ 72.5 ms → 13.8 Hz)。不再叠加 sr/25 的
    // 25 Hz 下限: 24 Hz 正弦的真实周期峰 (41.67 ms) 会落在该下限之外, 范围内
    // ACF 处于单调上升段, 窗口内容的任何扰动都会在截止边缘制造伪峰 ——
    // 表现为"有时锁上(锁到偏短的假周期)有时锁不上"。
    const int minLag = std::max(10, (int)std::round(mSampleRate / 2500.0)); // 2500 Hz 上限
    const int maxLag = std::min(N - 4, (int)(0.85f * N));

    const float invE = 1.0f / energy0;
    float *acf = mAcfBuf.data();
    for (int tau = 0; tau <= maxLag + 1; ++tau) {
      const float bias = (float)N / (float)(N - tau);
      acf[tau] = std::clamp(re[tau] * invE * bias, -1.0f, 1.0f);
    }

    // 跨过主瓣下降区
    int valleyLag = 1;
    while (valleyLag < maxLag && acf[valleyLag] > 0.3f && acf[valleyLag] <= acf[valleyLag - 1]) {
      ++valleyLag;
    }

    // 峰搜索: 先找最高峰, 再在其值 −margin 内取最短延迟。
    // margin 不可省: 无偏归一化后严格周期信号的 k 倍周期峰会全部 ≈1.0, 纯取
    // 全局最大会在基波与倍频间随机跳动 (方波曾锁到 5× 基波周期)。
    constexpr float kThresh = 0.35f; // 最低自相关置信门限
    constexpr float kMargin = 0.10f;

    const int searchFrom = std::max(minLag, valleyLag);
    int bestLag = -1;
    float bestPeak = kThresh;
    for (int lag = searchFrom; lag < maxLag; ++lag) {
      if (acf[lag] > bestPeak && acf[lag] > acf[lag - 1] && acf[lag] >= acf[lag + 1]) {
        bestPeak = acf[lag];
        bestLag = lag;
      }
    }

    if (bestLag > 0) {
      // margin 内的最短峰（严格周期信号回退到基波）
      const float cut = bestPeak - kMargin;
      for (int lag = searchFrom; lag < bestLag; ++lag) {
        if (acf[lag] >= cut && acf[lag] > acf[lag - 1] && acf[lag] >= acf[lag + 1]) {
          bestLag = lag;
          break;
        }
      }

      // 二次抛物线亚采样插值
      const float y0 = acf[bestLag - 1];
      const float y1 = acf[bestLag];
      const float y2 = acf[bestLag + 1];
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
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
