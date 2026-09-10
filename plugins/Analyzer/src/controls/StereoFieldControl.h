#pragma once

// StereoFieldControl — 立体声声像显示 (PAZ Position 式极坐标矢量线)

#include "IControls.h"
#include "ISender.h"
#include "UiUtils.h"
#include "../Theme.h"
#include "../dsp/FastMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class StereoFieldControl : public IControl {
public:
  using TDataPacket = std::array<float, 1024>;

  enum MsgTags {
    kMsgTagSampleRate = 1,
    kMsgTagRelease,
    kMsgTagReleaseMode,
    kMsgTagReset,
  };

  explicit StereoFieldControl(const IRECT &bounds) : IControl(bounds) {}

  // RESET 按钮联动: 清空矢量线与相关性窗口
  void ClearPeakHold() { ResetDisplay(); }

  void OnMsgFromDelegate(int msgTag, int dataSize, const void *pData) override {
    IByteStream stream(pData, dataSize);

    if (msgTag == ISender<>::kUpdateMessage) {
      if (dataSize != (int)sizeof(ISenderData<2, TDataPacket>))
        return;
      ISenderData<2, TDataPacket> d;
      std::memcpy(&d, pData, sizeof(d));
      ProcessPacket(d);
    } else if (msgTag == kMsgTagSampleRate) {
      stream.Get(&mSampleRate, 0);
    } else if (msgTag == kMsgTagRelease) {
      float v;
      stream.Get(&v, 0);
      mReleaseSec = std::clamp(v, 0.01f, 10.f);
    } else if (msgTag == kMsgTagReleaseMode) {
      int v;
      stream.Get(&v, 0);
      mReleaseMode = std::clamp(v, 0, 1);
    } else if (msgTag == kMsgTagReset) {
      ResetDisplay();
    }
  }

  // hover 指针
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
    g.FillRect(COL_100(), mRECT);
    IRECT skipRect, labelBox;
    const bool hov = ComputeHover(g, mRECT, labelBox);
    skipRect = hov ? labelBox : IRECT();
    DrawGridLayer(g, mRECT);
    DrawTicks(g, mRECT, skipRect);
    DrawVectors(g, mRECT);
    DrawBalanceBar(g, mRECT);
    if (hov)
      DrawHover(g, mRECT, labelBox);
  }

private:
  static constexpr int kFftN = 1024;          // hop = 1024 样本
  static constexpr int kScopeBands = 16;      // 对数频带数 (~0.56 倍频程/带)
  static constexpr int kCorrWin = 64;         // 相关性滑窗 hop 数 (~1.4s @48k)
  static constexpr float kFftAttackSec = 0.003f;
  static constexpr float kAntiWinFrames = 4.f;
  static constexpr int kImgBins = 64;         // 方位直方图 bin 数 (~2.8°/bin)
  static inline const float kImgStep = (float)PI / (float)kImgBins;
  static constexpr float kImgKernelBins = 1.f;
  static constexpr float kImgDecaySec = 0.7f;
  static constexpr float kImgDomeFrac = 0.10f;
  static constexpr float kImgAntiPow = 2.f;
  static constexpr float kImgWinFrames = 8.f;
  static constexpr double kFftCal = 0.375 * (double)kFftN * (double)kFftN; // 满幅正弦 = 0 dB

  static constexpr float kBarH = 14.f;       // 复合条轨道厚度 (L/R 内嵌其中, 需容纳文字)
  static constexpr float kReadoutH = 22.f;   // 基线下 CORRELATION/BALANCE 读数行高
  // 基线以下固定占用: 横条 + 间距 + 读数行 + 底缘边距, 供扇形半径计算预留
  static constexpr float kBelowH = kBarH + 4.f + kReadoutH + 2.f;
  static constexpr float kBalRangeDb = 24.f; // 平衡量程 ±24 dB
  static constexpr float kCxN = 0.28f;

  // 16 个对数频带的分 bin 表 (FFT 1024 @48k, ~0.56 倍频程/带)
  static constexpr int kBandBins[kScopeBands][2] = {
      {1, 1},   {2, 2},   {3, 3},   {4, 4},   {5, 7},   {8, 10},   {11, 15},  {16, 22},
      {23, 33}, {34, 49}, {50, 72}, {73, 107}, {108, 158}, {159, 234}, {235, 345}, {346, 510}};

  void ProcessPacket(const ISenderData<2, TDataPacket> &d) {
    const double hopSec = 1024.0 / std::max(mSampleRate, 1.0);
    const float aCoef = (float)std::exp(-hopSec / kFftAttackSec);
    const float rCoef = (float)std::exp(-hopSec / std::max(mReleaseSec, 1e-3f));
    const float unifStep = (float)(hopSec / (2.0 * std::max(mReleaseSec, 1e-3f)) * -mFloorDb);

    // L/R 1024 点 FFT (加 Hann 窗)
    std::array<float, kFftN> reL, imL, reR, imR;
    const float *inL = d.vals[0].data();
    const float *inR = d.vals[1].data();
    for (int i = 0; i < kFftN; ++i) {
      const float w = 0.5f - 0.5f * std::cos(2.f * (float)PI * i / (float)kFftN);
      reL[i] = inL[i] * w; imL[i] = 0.f;
      reR[i] = inR[i] * w; imR[i] = 0.f;
    }
    FftRadix2(reL.data(), imL.data(), kFftN);
    FftRadix2(reR.data(), imR.data(), kFftN);

    // 分带聚合
    double eInB[kScopeBands] = {}, dlInB[kScopeBands] = {}, rcInB[kScopeBands] = {};
    double eAntiB[kScopeBands] = {}, dlAntiB[kScopeBands] = {}, rcAntiB[kScopeBands] = {};
    for (int b = 0; b < kScopeBands; ++b) {
      for (int k = kBandBins[b][0]; k <= kBandBins[b][1]; ++k) {
        const double l2 = reL[k] * reL[k] + imL[k] * imL[k];
        const double r2 = reR[k] * reR[k] + imR[k] * imR[k];
        const double rck = reL[k] * reR[k] + imL[k] * imR[k];
        if (rck >= 0.0) {
          eInB[b] += l2 + r2;
          dlInB[b] += r2 - l2;
          rcInB[b] += rck;
        } else {
          eAntiB[b] += l2 + r2;
          dlAntiB[b] += r2 - l2;
          rcAntiB[b] += rck;
        }
      }
    }
    double eB[kScopeBands], dlB[kScopeBands], rcB[kScopeBands];
    for (int b = 0; b < kScopeBands; ++b) {
      eB[b] = eInB[b] + eAntiB[b];
      dlB[b] = dlInB[b] + dlAntiB[b];
      rcB[b] = rcInB[b] + rcAntiB[b];
    }

    // 相关性/平衡: kCorrWin hop 滑窗 + 150ms 显示平滑
    double lr = 0.0, l2 = 0.0, r2 = 0.0;
    for (int i = 0; i < kFftN; ++i) {
      const double l = inL[i], r = inR[i];
      lr += l * r;
      l2 += l * l;
      r2 += r * r;
    }
    const HopSum &old = mCorrRing[mCorrHead];
    mSumLR += lr - old.lr;
    mSumL2 += l2 - old.l2;
    mSumR2 += r2 - old.r2;
    mCorrRing[mCorrHead] = {lr, l2, r2};
    mCorrHead = (mCorrHead + 1) & (kCorrWin - 1);
    const double winE = mSumL2 + mSumR2;
    if (winE > 1e-10) {
      const float corr =
          (float)std::clamp(mSumLR / std::sqrt(std::max(mSumL2 * mSumR2, 1e-24)), -1.0, 1.0);
      const float balDb = orm::FastPwrToDb((float)(mSumR2 / std::max(mSumL2, 1e-12)), -120.f);
      const float sm = (float)(1.0 - std::exp(-hopSec / 0.15));
      mCorrDisp += sm * (corr - mCorrDisp);
      mBalDisp += sm * (balDb - mBalDisp);
      mCorrValid = true;
    } else {
      mCorrValid = false;
    }

    // 每带: 电平弹道 + 连续方位 + 分组窗口更新
    for (int b = 0; b < kScopeBands; ++b) {
      const double e = eB[b];
      const float rawDb = (e > 1e-12) ? orm::FastPwrToDb((float)(e / kFftCal), -120.f) : -120.f;
      const float target = std::clamp(rawDb, mFloorDb - 0.1f * -mFloorDb, 0.f);
      mBand[b].db = StepSmoothed(mBand[b].db, target, aCoef, rCoef, unifStep);

      mBand[b].ang = (e > 1e-12) ? 0.5f * (float)std::atan2(dlB[b], 2.0 * rcB[b]) : 0.f;

      if (e > 1e-12) {
        mRcWin[b] += (rcB[b] - mRcWin[b]) / (double)kAntiWinFrames;
        mEWin[b] += (e - mEWin[b]) / (double)kAntiWinFrames;
      }
      if (eInB[b] > 1e-12) {
        mDlInWin[b] += (dlInB[b] - mDlInWin[b]) / (double)kImgWinFrames;
        mRcInWin[b] += (rcInB[b] - mRcInWin[b]) / (double)kImgWinFrames;
      }
    }

    // 方位直方图: 每 hop 衰减，同相组直接注入 (中心锥)，反相组按相关负度加权
    const float imgCoef = (float)std::exp(-hopSec / kImgDecaySec);
    for (int i = 0; i < kImgBins; ++i)
      mImg[i] *= imgCoef;
    auto injectLvl = [&](float th, float db) {
      if (db <= mFloorDb + 0.5f)
        return;
      const float rNorm = RadiusFor(db, 1.f);
      const float fc = (th + 0.5f * (float)PI) / kImgStep - 0.5f;
      const int i0 = std::max(0, (int)std::ceil(fc - kImgKernelBins));
      const int i1 = std::min(kImgBins - 1, (int)std::floor(fc + kImgKernelBins));
      for (int i = i0; i <= i1; ++i) {
        const float k = 0.5f * (1.f + std::cos((float)PI * std::fabs((float)i - fc) / kImgKernelBins));
        mImg[i] = std::max(mImg[i], rNorm * k);
      }
    };
    auto groupTh = [](float dl, float rc) {
      return std::clamp(0.5f * std::atan2(dl, 2.0f * rc), -0.5f * (float)PI, 0.5f * (float)PI);
    };
    for (int b = 0; b < kScopeBands; ++b) {
      if (eInB[b] > 1e-12)
        injectLvl(groupTh((float)mDlInWin[b], (float)mRcInWin[b]),
                  orm::FastPwrToDb((float)(eInB[b] / kFftCal), -120.f));
      if (eB[b] > 1e-12 && mEWin[b] > 1e-12) {
        const float w = (float)std::pow(
            std::clamp((float)(-mRcWin[b] / (0.5 * mEWin[b])), 0.f, 1.f), kImgAntiPow);
        if (w > 1e-4f)
          injectLvl(groupTh((float)dlAntiB[b], (float)rcAntiB[b]),
                    orm::FastPwrToDb((float)(eAntiB[b] / kFftCal), -120.f) + 20.f * std::log10(w));
      }
    }
    SetDirty(false);
  }

  float StepSmoothed(float prevDb, float targetDb, float aCoef, float rCoef, float unifStepDb) const {
    if (targetDb > prevDb)
      return aCoef * prevDb + (1.f - aCoef) * targetDb;
    if (mReleaseMode == 1)
      return std::max(targetDb, prevDb - unifStepDb);
    return rCoef * prevDb + (1.f - rCoef) * targetDb;
  }

  void ResetDisplay() {
    mBand = {};
    mImg.fill(0.f);
    mRcWin.fill(0.0);
    mEWin.fill(0.0);
    mDlInWin.fill(0.0);
    mRcInWin.fill(0.0);
    mCorrRing.fill(HopSum{});
    mCorrHead = 0;
    mSumLR = mSumL2 = mSumR2 = 0.0;
    mCorrValid = false;
    mCorrDisp = mBalDisp = 0.f;
    SetDirty(false);
  }

  // 扇形弧顶贴画区顶缘 (与示波器按钮排顶对齐), 基线 = 顶缘 + 半径; 左右各留 2px 与频谱画区同款
  void FanGeom(const IRECT &cv, float &cx, float &cy, float &rMax) const {
    cx = (cv.W() <= 500.f) ? cv.MW() : (cv.L + cv.W() * kCxN);
    const float rHoriz = std::max(cv.W() * 0.5f - 2.f, 12.f);
    const float rVert = std::max(cv.B - kBelowH - cv.T, 12.f);
    rMax = (cv.W() <= 500.f) ? std::min(rVert, rHoriz) : rVert;
    cy = cv.T + rMax;
  }

  float RadiusFor(float db, float rMax) const {
    return rMax * std::clamp((db - mFloorDb) / (0.f - mFloorDb), 0.f, 1.f);
  }

  // 语义色
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

  void DrawGridLayer(IGraphics &g, const IRECT &cv) {
    const int hue = ThemeHue(), sat = ThemeSatMax(), mode = ThemeMode();
    if (!g.CheckLayer(mGridLayer) || mFloorDb != mGridFloor || hue != mGridHue || sat != mGridSat ||
        mode != mGridMode) {
      g.StartLayer(this, cv);
      DrawGridContent(g, cv);
      mGridLayer = g.EndLayer();
      mGridFloor = mFloorDb;
      mGridHue = hue;
      mGridSat = sat;
      mGridMode = mode;
    }
    g.DrawLayer(mGridLayer);
  }

  // 色块
  void DrawGridContent(IGraphics &g, const IRECT &cv) {
    float cx, cy, rMax;
    FanGeom(cv, cx, cy, rMax);
    const int floorInt = (int)mFloorDb;
    const float kDeg = (float)PI / 180.f;
    for (int j = 0;; ++j) {
      const int dbHi = -20 * j;
      if (dbHi <= floorInt)
        break;
      const int dbLo = std::max(dbHi - 20, floorInt);
      const float rHi = RadiusFor((float)dbHi, rMax);
      const float rLo = RadiusFor((float)dbLo, rMax);
      const float tRow = (float)(0 - (dbHi + dbLo) * 0.5) / (float)(0 - floorInt);
      const float vDb = 245.f + (172.f - 245.f) * tRow;
      for (int i = 0; i < 12; ++i) {
        const float a0 = (-90.f + 15.f * i) * kDeg;
        const float a1 = a0 + 15.f * kDeg;
        const float aa = std::fabs(a0 + 7.5f * kDeg);
        const float vAng = (aa <= 15.f * kDeg) ? 230.f : (aa <= 45.f * kDeg) ? 215.f : 195.f;
        const int v = (int)std::lround(0.5f * (vAng + vDb));
        FillSector(g, cx, cy, rLo, rHi, a0, a1, GridGray(v));
      }
    }
  }

  // 扇形填充
  static void FillSector(IGraphics &g, float cx, float cy, float rLo, float rHi, float a0, float a1,
                         const IColor &c) {
    if (rHi - rLo <= 0.5f)
      return;
    constexpr float kPad = 0.003f;
    constexpr int kSegs = 4;
    rLo = std::max(rLo - 0.75f, 0.f);
    a0 -= kPad;
    a1 += kPad;
    g.PathClear();
    if (rLo <= 1.f) {
      g.PathMoveTo(cx, cy);
    } else {
      for (int k = 0; k <= kSegs; ++k) {
        const float a = a0 + (a1 - a0) * (float)k / (float)kSegs;
        const float x = cx + std::sin(a) * rLo, y = cy - std::cos(a) * rLo;
        if (k == 0)
          g.PathMoveTo(x, y);
        else
          g.PathLineTo(x, y);
      }
    }
    for (int k = kSegs; k >= 0; --k) {
      const float a = a0 + (a1 - a0) * (float)k / (float)kSegs;
      g.PathLineTo(cx + std::sin(a) * rHi, cy - std::cos(a) * rHi);
    }
    g.PathClose();
    g.PathFill(IPattern(c));
  }

  // 场内标注: L/R 水印字居 ±15°~±45° 扇区最外圈 (仿 BandPass 频谱侧标样式), Anti Phase 在 ±90° 基线两端，dB 刻度沿中轴
  void DrawTicks(IGraphics &g, const IRECT &cv, const IRECT &skipRect) {
    float cx, cy, rMax;
    FanGeom(cv, cx, cy, rMax);

    auto draw = [&](const IText &it, const char *txt, const IRECT &box) {
      if (!skipRect.Empty() && box.Intersects(skipRect))
        return;
      g.DrawText(it, txt, box);
    };

    // L/R 方向字: 居扇区角中值 ±30°、最外圈内侧, 字面沿半径朝外旋转 (旋转轴过圆心)
    constexpr float kLrDeg = 30.f;   // 扇区 −45°~−15° / 15°~45° 的角中值
    constexpr float kLrSize = 40.f;  // 同 BandPass 侧标字号
    constexpr float kLrPad = 26.f;   // 字心到最外圈的距离 (字高一半 + 余量)
    const float thRad = kLrDeg * (float)M_PI / 180.f;
    const float rLbl = rMax - kLrPad;
    for (int side = 0; side < 2; ++side) {
      const bool isL = (side == 0);
      const float th = (isL ? -1.f : 1.f) * thRad;
      const IText t(kLrSize, COL_500(), kFontBold, EAlign::Center, EVAlign::Middle,
                    th * 180.f / (float)M_PI); // IText 正角为顺时针(屏幕 y 向下): 字面朝外旋转 θ
      const float px = cx + std::sin(th) * rLbl;
      const float py = cy - std::cos(th) * rLbl;
      IRECT mr;
      g.MeasureText(t, isL ? "L" : "R", mr);
      draw(t, isL ? "L" : "R",
           IRECT(px - mr.W() * 0.5f, py - mr.H() * 0.5f, px + mr.W() * 0.5f, py + mr.H() * 0.5f));
    }

    const IText apT(14, COL_700(), kFontRegular, EAlign::Center, EVAlign::Middle);
    const float ax = rMax - 50.f;
    draw(apT, "Anti Phase", IRECT(cx - ax - 36.f, cy - 19.f, cx - ax + 36.f, cy - 5.f));
    draw(apT, "Anti Phase", IRECT(cx + ax - 36.f, cy - 19.f, cx + ax + 36.f, cy - 5.f));

    const IText dbT(14, COL_700(), kFontRegular, EAlign::Near, EVAlign::Top);
    const int floorInt = (int)mFloorDb;
    for (int db = 0; db >= floorInt; db -= 20) {
      const float r = RadiusFor((float)db, rMax);
      if (r <= 20.f)
        continue;
      const float y = cy - r;
      const IRECT box(cx + 5.f, y + 1.f, cx + 48.f, y + 17.f);
      if (!skipRect.Empty() && box.Intersects(skipRect))
        continue;
      char buf[8];
      std::snprintf(buf, sizeof(buf), "%d", db);
      g.DrawText(dbT, buf, box);
    }
  }

  // 方位直方图
  void DrawVectors(IGraphics &g, const IRECT &cv) {
    float cx, cy, rMax;
    FanGeom(cv, cx, cy, rMax);
    IColor cL, cR, cM;
    GetChannelColors(cL, cR, cM);

    float maxImg = 0.f;
    for (int i = 0; i < kImgBins; ++i)
      maxImg = std::max(maxImg, mImg[i]);
    if (maxImg <= 1e-3f)
      return;

    const float rDome = std::max(3.f, rMax * kImgDomeFrac);
    const float rPeak = std::max(maxImg * rMax, rDome + 1.f);

    // 径向渐变: alpha = 15 + 240·exp(-3.5(1-s))，穹顶最透，峰值实色
    IPattern fill = IPattern::CreateRadialGradient(cx, cy, rPeak);
    {
      constexpr int kN = 8;
      constexpr float kMinA = 15.f;
      for (int i = 0; i < kN; ++i) {
        const float s = (float)i / (float)(kN - 1);
        const float w = std::exp(-3.5f * (1.f - s));
        const int a = (int)std::lround(kMinA + (255.f - kMinA) * w);
        fill.AddStop(IColor(a, cM.R, cM.G, cM.B), (rDome + (rPeak - rDome) * s) / rPeak);
      }
    }

    g.PathClear();
    for (int i = 0; i < kImgBins; ++i) {
      const float th = -0.5f * (float)PI + (i + 0.5f) * kImgStep;
      const float r = std::max(mImg[i] * rMax, rDome);
      const float x = cx + std::sin(th) * r;
      const float y = cy - std::cos(th) * r;
      if (i == 0)
        g.PathMoveTo(x, y);
      else
        g.PathLineTo(x, y);
    }
    g.PathLineTo(cx + rDome, cy);
    g.PathLineTo(cx - rDome, cy);
    g.PathClose();
    g.PathFill(fill);
  }

  // 相关性状态色
  static IColor CorrColor(float c) {
    return (c >= 0.3f)   ? SemColor(MeterGreen())
           : (c >= 0.f)  ? SemColor(MeterYellow())
                         : SemColor(MeterRed());
  }

  // 复合条: 相关性单声道窗口 + ±24dB 平衡指针, 与半圆基线无缝相接, 宽度 = 半圆直径
  void DrawBalanceBar(IGraphics &g, const IRECT &cv) {
    float cx, cy, rMax;
    FanGeom(cv, cx, cy, rMax);
    const float half = std::min(rMax, std::min(cx - cv.L - 2.f, cv.R - cx - 2.f));
    const float x0 = cx - half, x1 = cx + half;
    const float trackT = cy, trackB = trackT + kBarH;
    const float roT = trackB + 4.f, roB = roT + kReadoutH;
    IColor cL, cR, cM;
    GetChannelColors(cL, cR, cM);

    // 轨道 (下缘不再带 ±6dB 小刻度)
    g.FillRect(COL_300(), IRECT(x0, trackT, x1, trackB));

    if (mCorrValid) {
      const float c = std::clamp(mCorrDisp, -1.f, 1.f);
      const IColor cc = CorrColor(c);

      // 单声道窗口: 半宽 = |corr|·半条宽, 负相关转红; 只保留颜色填充, 不画中心基准竖条
      const float halfW = 0.5f * std::fabs(c) * (x1 - x0);
      g.FillRect(IColor(150, cc.R, cc.G, cc.B), IRECT(cx - halfW, trackT, cx + halfW, trackB));

      // 平衡指针: 不再填充通道色, 仅一根深灰竖条, 与轨道上下同高
      const float bal = std::clamp(mBalDisp, -kBalRangeDb, kBalRangeDb);
      const float nx = cx + (bal / kBalRangeDb) * half;
      g.FillRect(COL_700(), IRECT(nx - 1.5f, trackT, nx + 1.5f, trackB));
    }

    // L/R 标识内嵌轨道两端 (通道色), 不再显示中心 0
    const IText lT(12, IColor(255, cL.R, cL.G, cL.B), kFontSemiBold, EAlign::Near, EVAlign::Middle);
    const IText rT(12, IColor(255, cR.R, cR.G, cR.B), kFontSemiBold, EAlign::Far, EVAlign::Middle);
    g.DrawText(lT, "L", IRECT(x0 + 5.f, trackT, x0 + 25.f, trackB));
    g.DrawText(rT, "R", IRECT(x1 - 25.f, trackT, x1 - 5.f, trackB));

    // 两角读数: 左 CORRELATION (状态色), 右 BALANCE; 全称随界面语言切换
    const char *corrLbl = orm::Tr(orm::kTxtCorr, orm::UILang());
    const char *balLbl = orm::Tr(orm::kTxtBalance, orm::UILang());
    char buf[48];
    if (mCorrValid) {
      std::snprintf(buf, sizeof(buf), "%s  %+.2f", corrLbl, mCorrDisp);
      g.DrawText(IText(16, CorrColor(std::clamp(mCorrDisp, -1.f, 1.f)), kFontSemiBold, EAlign::Near,
                       EVAlign::Middle),
                 buf, IRECT(x0, roT, x0 + 150.f, roB));
      if (std::fabs(mBalDisp) < 0.1f)
        std::snprintf(buf, sizeof(buf), "%s  C", balLbl);
      else
        std::snprintf(buf, sizeof(buf), "%s  %s %.1f dB", balLbl, (mBalDisp > 0.f) ? "R" : "L",
                      std::fabs(mBalDisp));
      g.DrawText(IText(16, COL_900(), kFontSemiBold, EAlign::Far, EVAlign::Middle), buf,
                 IRECT(x1 - 160.f, roT, x1, roB));
    } else {
      const IText dimT(16, COL_500(), kFontSemiBold, EAlign::Near, EVAlign::Middle);
      const IText dimTF(16, COL_500(), kFontSemiBold, EAlign::Far, EVAlign::Middle);
      std::snprintf(buf, sizeof(buf), "%s  —", corrLbl);
      g.DrawText(dimT, buf, IRECT(x0, roT, x0 + 150.f, roB));
      std::snprintf(buf, sizeof(buf), "%s  —", balLbl);
      g.DrawText(dimTF, buf, IRECT(x1 - 160.f, roT, x1, roB));
    }
  }

  // hover 径向指针 + 复合读数
  bool ComputeHover(IGraphics &g, const IRECT &cv, IRECT &labelBox) {
    labelBox = IRECT();
    if (!mHoverActive)
      return false;
    float cx, cy, rMax;
    FanGeom(cv, cx, cy, rMax);
    const float dx = mHoverX - cx, dyUp = cy - mHoverY;
    if (mHoverY > cy || dx * dx + dyUp * dyUp > rMax * rMax + 4.f)
      return false;
    mHoverTh = std::atan2(dx, std::max(dyUp, 0.001f));

    float maxDb = mFloorDb;
    for (int b = 0; b < kScopeBands; ++b)
      maxDb = std::max(maxDb, mBand[b].db);
    const float thr = std::max(maxDb - 35.f, mFloorDb + 0.5f);
    float best = 1e9f;
    int bi = -1;
    for (int b = 0; b < kScopeBands; ++b) {
      if (mBand[b].db < thr)
        continue;
      const float d = std::fabs(mBand[b].ang - mHoverTh);
      if (d < best) {
        best = d;
        bi = b;
      }
    }
    if (bi >= 0 && best < 0.14f) {
      const float hz = (float)(0.5 * (kBandBins[bi][0] + kBandBins[bi][1])) * mSampleRate / kFftN;
      char fb[16];
      if (hz < 1000.f)
        std::snprintf(fb, sizeof(fb), "%.0f Hz", hz);
      else
        std::snprintf(fb, sizeof(fb), "%.2f kHz", hz / 1000.f);
      std::snprintf(mHoverBuf, sizeof(mHoverBuf), "%.1f°  %s  %.1f dB",
                    mHoverTh * 180.f / (float)PI, fb, mBand[bi].db);
    } else {
      std::snprintf(mHoverBuf, sizeof(mHoverBuf), "%.1f°", mHoverTh * 180.f / (float)PI);
    }

    // 标签沿指针方向放弧缘外侧，超出画布时平移回画布内 (弧顶已贴顶缘, 纵向一并收紧)
    const IText t(14, COL_700(), kFontRegular, EAlign::Center, EVAlign::Bottom);
    const float px = cx + (rMax + 18.f) * std::sin(mHoverTh);
    const float py = std::max(cy - (rMax + 18.f) * std::cos(mHoverTh), cv.T + 17.f);
    IRECT box(px - 60.f, py - 17.f, px + 60.f, py - 1.f);
    g.MeasureText(t, mHoverBuf, box);
    const float w = box.W();
    const float cxL = std::clamp(px - w * 0.5f, cv.L + 2.f, cv.R - 2.f - w);
    labelBox = IRECT(cxL, py - 17.f, cxL + w, py - 1.f);
    return true;
  }

  void DrawHover(IGraphics &g, const IRECT &cv, const IRECT &labelBox) {
    float cx, cy, rMax;
    FanGeom(cv, cx, cy, rMax);
    g.DrawLine(COL_700(), cx, cy, cx + std::sin(mHoverTh) * rMax, cy - std::cos(mHoverTh) * rMax,
               nullptr, 1.f);
    g.DrawText(IText(14, COL_700(), kFontRegular, EAlign::Near, EVAlign::Bottom), mHoverBuf, labelBox);
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

  struct Band {
    float db = -120.f;  // 带电平 (显示域弹道 dB)
    float ang = 0.f;    // 连续方位 (弧度)
  };
  std::array<Band, kScopeBands> mBand{};
  std::array<float, kImgBins> mImg{}; // 方位直方图 (0..1 归一化电平)

  struct HopSum {
    double lr, l2, r2;
  };
  std::array<HopSum, kCorrWin> mCorrRing{};
  int mCorrHead = 0;
  double mSumLR = 0.0, mSumL2 = 0.0, mSumR2 = 0.0;
  bool mCorrValid = false;
  float mCorrDisp = 0.f, mBalDisp = 0.f;

  double mSampleRate = 48000.0;
  float mReleaseSec = 0.2f;
  int mReleaseMode = 0; // 0=LOG, 1=LIN 匀速
  float mFloorDb = -80.f;

  std::array<double, kScopeBands> mRcWin{}, mEWin{};      // 反相权重窗
  std::array<double, kScopeBands> mDlInWin{}, mRcInWin{}; // 同相组方位窗
  char mHoverBuf[64] = "";

  bool mHoverActive = false;
  float mHoverX = 0.f, mHoverY = 0.f;
  float mHoverTh = 0.f;

  // 静态网格离屏缓存
  ILayerPtr mGridLayer;
  float mGridFloor = -1000.f;
  int mGridHue = -1;
  int mGridSat = -1;
  int mGridMode = -1;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
