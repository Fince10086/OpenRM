#pragma once

// StereoFieldControl — 立体声声像显示 (PAZ Position 式极坐标矢量线)
//
// 位于频谱下方空闲区 (布局 20,336 → 752,604)。数据管线与频谱同构:
// StereoScope 引擎在音频线程只攒 1024 样本 hop 原始包 → ISender 队列 →
// OnIdle TransmitData → 本控件 OnMsgFromDelegate; FFT/分带/弹道全部在 UI
// 线程完成 (重活不过音频线程)。
//
// 显示制式对齐 Waves PAZ Position (SPD): 极坐标矢量图 —— 每个显示元素是
// 一条从圆心发出的射线, 长度 = 响度 (dB 半径刻度), 角度 = 声像位置; 显示
// 角度 = 模型方位角折半 (δ=θ/2): 同相 |θ|≤90° → 中央 ±45°, 反相 → 侧翼
// 45°..90°, 硬反相 → 基线两端, 整幅正好 180° 半扇 (网格与读数栏沿用)。
//
// 每 hop 对 L/R 做 1024 点 FFT, 按 16 个对数频带聚合 (Σ|L|², Σ|R|²,
// ΣRe(L·conj R)) → 每带一条主射线 (帧级方位 + 电平弹道)。反相带 (相关
// < −0.2) 的主射线角度 = 帧级 (平衡, 相关) 的自然半角映射, 连续覆盖
// ±45°..±90° 整个侧翼 (PAZ: 反相信号显示在每侧 45°..90° 之间), 硬反相
// 时 ±90° 在帧间交替点亮两侧; 非反相的去相关内容在 ±90° 补两条侧翼
// 射线。复现 PAZ 观感: 静态单音一条线, 90° 相位差音正中 + ±90° 三条
// 线, 双频双方位两条线, 反相谐波两侧 90°, 噪声三段区域持续跳动。
//
// 电平弹道攻击基本直跳 (3ms, 快速扫过的线不丢电平, 噪声线长自然"跳动",
// 对应 PAZ Peak 响度观感), 释放沿用 LOG/LIN 档位; 主射线角度 30ms 平滑、
// 侧翼比例 100ms 平滑 (侧翼跳动略慢, 对应参考观感)。HOLD/峰值保持与
// hover 在矢量制式下不再有意义, 已移除 (消息接口保留, RESET 仍清空显示状态)。
// 右栏为相关性/宽度/平衡读数 (64 hop ≈1.4s 滑窗 + 150ms 显示平滑)。

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
  // 与 StereoScope 引擎的数据包严格一致 (ISender 数据包整体拷贝, 不一致会整包读取失败)
  using TDataPacket = std::array<float, 1024>;

  enum MsgTags {
    kMsgTagSampleRate = 1,
    kMsgTagAttack,
    kMsgTagRelease,
    kMsgTagReleaseMode,
    kMsgTagRange, // 显示 dB 底限 (float, 负值)
    kMsgTagHold,  // 峰值保持时长 (s, 0 = 关) —— 矢量制式下保留接口, 不再绘制
    kMsgTagReset, // 清空带状态/相关性窗口 (配置重建/冻结回放前)
  };

  explicit StereoFieldControl(const IRECT &bounds) : IControl(bounds) {}

  // RESET 按钮联动: 清空矢量线与相关性窗口
  void ClearPeakHold() { ResetDisplay(); }

  // 翻译文字 (ApplyLanguage 绑定; Tr() 返回静态表指针, 可直接暂存)
  void SetTitleText(const char *s) { mTitle = s; SetDirty(false); }
  void SetCorrLabel(const char *s) { mCorrLabel = s; SetDirty(false); }
  void SetWidthLabel(const char *s) { mWidthLabel = s; SetDirty(false); }
  void SetBalanceLabel(const char *s) { mBalanceLabel = s; SetDirty(false); }
  void SetAntiLabel(const char *s) { mAntiLabel = s; SetDirty(false); }

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
    } else if (msgTag == kMsgTagAttack) {
      float v;
      stream.Get(&v, 0);
      mAttackSec = std::clamp(v, 0.001f, 0.1f);
    } else if (msgTag == kMsgTagRelease) {
      float v;
      stream.Get(&v, 0);
      mReleaseSec = std::clamp(v, 0.01f, 8.f); // LIN 最慢档释放达 4.8s
    } else if (msgTag == kMsgTagReleaseMode) {
      int v;
      stream.Get(&v, 0);
      mReleaseMode = std::clamp(v, 0, 1);
    } else if (msgTag == kMsgTagRange) {
      float v;
      stream.Get(&v, 0);
      mFloorDb = std::clamp(v, -120.f, -30.f);
    } else if (msgTag == kMsgTagHold) {
      float v;
      stream.Get(&v, 0);
      mHoldSec = v; // 保留接口 (无视觉)
    } else if (msgTag == kMsgTagReset) {
      ResetDisplay();
    }
  }

  void Draw(IGraphics &g) override {
    g.FillRect(COL_100(), mRECT);
    const IRECT cv = Canvas();
    DrawGridLayer(g, cv);
    DrawVectors(g, cv);
    DrawReadouts(g, cv);
  }

private:
  // ── 常量 ──────────────────────────────────────────────────────────────
  static constexpr int kFftN = 1024;          // hop = 1024 样本, 每包一次 FFT
  static constexpr int kScopeBands = 16;      // 对数频带数 (≈0.56 倍频程/带, 见 bin 表)
  static constexpr int kCorrWin = 64;         // 相关性滑窗 hop 数 (~1.4s @48k, 2 的幂)
  static constexpr float kFftAttackSec = 0.003f; // 电平攻击 (基本直跳, PAZ Peak 响度观感)
  static constexpr float kAngSmoothSec = 0.030f; // 主射线角度平滑
  static constexpr float kWingSmoothSec = 0.100f;// 侧翼比例平滑 (略慢 → 两侧跳动缓于中间)
  static constexpr float kDecorrTol = 0.15f;  // 去相关歧义阈值: |bal|,|corr| 均低于它 → 居中
  static constexpr float kAntiThresh = -0.2f; // 相关低于此判"反相带" (微小负相关保持三线模式)
  static constexpr float kHeadH = 22.f;       // 顶部标题行高
  static constexpr float kBasePad = 20.f;     // 基线距画布底缘留白 (L/R 标注带)
  static constexpr float kDomePad = 8.f;      // 穹顶距画布上/左右边缘留白

  // 16 个对数频带的分 bin 表 (含两端; FFT 1024 @48k 时 bin ≈ 46.875 Hz, 逐带
  // 约 ×1.477 ≈ 0.56 倍频程): 47 Hz..23.9 kHz 对数铺开, 足以分开相差一个
  // 倍频程的成分 (如 440/880 谐波对, 对齐 PAZ 的分带观感)。
  static constexpr int kBandBins[kScopeBands][2] = {
      {1, 1},   {2, 2},   {3, 3},   {4, 4},   {5, 7},   {8, 10},   {11, 15},  {16, 22},
      {23, 33}, {34, 49}, {50, 72}, {73, 107}, {108, 158}, {159, 234}, {235, 345}, {346, 510}};

  // ── 数据处理 ──────────────────────────────────────────────────────────
  // 每 hop 包: L/R FFT → 分带聚合 → 每带方位/电平弹道 + 相关性滑窗
  void ProcessPacket(const ISenderData<2, TDataPacket> &d) {
    const double hopSec = 1024.0 / std::max(mSampleRate, 1.0);
    // 弹道系数: 电平攻击近直跳 (3ms, 快扫不丢), 释放沿用 LOG/LIN 档位参数
    const float aCoef = (float)std::exp(-hopSec / kFftAttackSec);
    const float rCoef = (float)std::exp(-hopSec / std::max(mReleaseSec, 1e-3f));
    const float unifStep = (float)(hopSec / (2.0 * std::max(mReleaseSec, 1e-3f)) * -mFloorDb);
    const float angK = (float)(1.0 - std::exp(-hopSec / kAngSmoothSec));
    const float wingK = (float)(1.0 - std::exp(-hopSec / kWingSmoothSec));

    // L/R → 1024 点 FFT (Hann 窗, 抑制谱泄漏: 无窗时单音会漏进相邻所有带)
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

    // 分带帧级聚合: E = Σ(|L|²+|R|²), DL = Σ(|R|²−|L|²), LRC = ΣRe(L·conj R)
    double eB[kScopeBands] = {}, dlB[kScopeBands] = {}, rcB[kScopeBands] = {};
    for (int b = 0; b < kScopeBands; ++b) {
      for (int k = kBandBins[b][0]; k <= kBandBins[b][1]; ++k) {
        const double l2 = reL[k] * reL[k] + imL[k] * imL[k];
        const double r2 = reR[k] * reR[k] + imR[k] * imR[k];
        eB[b] += l2 + r2;
        dlB[b] += r2 - l2;
        rcB[b] += reL[k] * reR[k] + imL[k] * imR[k];
      }
    }

    // 相关性/宽度/平衡: kCorrWin hop 滑窗 (全带时域聚合) + 150ms 显示平滑
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
      const double s2 = std::max(0.5 * (winE - 2.0 * mSumLR), 1e-12);
      const double m2 = std::max(0.5 * (winE + 2.0 * mSumLR), 1e-12);
      const float widthDb = std::min(orm::FastPwrToDb((float)(s2 / m2), -120.f), 24.f);
      const float balDb = orm::FastPwrToDb((float)(mSumR2 / std::max(mSumL2, 1e-12)), -120.f);
      const float sm = (float)(1.0 - std::exp(-hopSec / 0.15));
      mCorrDisp += sm * (corr - mCorrDisp);
      mWidthDisp += sm * (widthDb - mWidthDisp);
      mBalDisp += sm * (balDb - mBalDisp);
      mCorrValid = true;
    } else {
      mCorrValid = false;
    }

    // 每带: 电平弹道 + 主射线方位 + 侧翼比例
    // 标定: Hann 窗 (Σw² ≈ 0.375·N) + Parseval → Σ_bins|X|² = N·Σ(w·x)²,
    // 满幅单声道正弦 = 0 dB
    const double kCal = 0.375 * (double)kFftN * (double)kFftN;
    for (int b = 0; b < kScopeBands; ++b) {
      const double e = eB[b];
      const float rawDb = (e > 1e-12) ? orm::FastPwrToDb((float)(e / kCal), -120.f) : -120.f;
      const float target = std::clamp(rawDb, mFloorDb - 0.1f * -mFloorDb, 0.f);
      mBand[b].db = StepSmoothed(mBand[b].db, target, aCoef, rCoef, unifStep);

      if (e > 1e-12) {
        const float bal = (float)(dlB[b] / e);
        const float corr = (float)(rcB[b] / (0.5 * e)); // 能量加权相关 (ΣLR/½E)
        // 主角度 = ½·atan2(Σ|R|²−Σ|L|², 2ΣRe(L·conj R)); 去相关歧义 (两者都≈0)
        // 时无确定方位 → 居中 (0°), 能量交给侧翼 ±90° (PAZ 三线观感)
        float raw;
        if (std::fabs(bal) < kDecorrTol && std::fabs(corr) < kDecorrTol)
          raw = 0.f;
        else
          raw = 0.5f * (float)std::atan2(dlB[b], 2.0 * rcB[b]);
        // 反相带 (corr < kAntiThresh): 角度直跳 (不做平滑 —— ±90° 在帧间交替,
        //  平滑会把它折中成 0°), 主射线自然落在 ±45°..±90° 连续域内, 覆盖整个
        //  侧翼 (PAZ: 反相信号显示在每侧 45°..90° 之间)。
        // 非反相带: 标量平滑 —— corr ≥ kAntiThresh 把角度域约束在 ±56° 内,
        //  无跨半扇端点的回绕, 数学上不可能出界。
        const bool anti = (corr < kAntiThresh);
        mBand[b].curAng = raw;
        if (!anti)
          mBand[b].ang += angK * (raw - mBand[b].ang);
        mBand[b].anti = anti;
        // 侧翼比例: 反相 → 满; 去相关 (相关低且居中) → 按 (1−corr)(1−2.2|bal|);
        // 硬声像 (|bal| 大) 或强相关 → 0 (单线, 与 PAZ 硬声像观感一致)
        const float wing =
            (corr < 0.f) ? 1.f
                         : std::max(0.f, (1.f - corr)) * std::max(0.f, (1.f - 2.2f * std::fabs(bal)));
        mBand[b].wing += wingK * (wing - mBand[b].wing);
      } else {
        mBand[b].ang = 0.f;
        mBand[b].curAng = 0.f;
        mBand[b].anti = false;
        mBand[b].wing *= (1.f - wingK); // 无能量时侧翼回落
      }
    }
    SetDirty(false);
  }

  // 单点弹道 (与频谱同式): 攻击 dB 域单极点; 回落按模式
  //   LOG = 显示域差距等比收缩, UNIF = 恒定屏幅比例速率
  float StepSmoothed(float prevDb, float targetDb, float aCoef, float rCoef, float unifStepDb) const {
    if (targetDb > prevDb)
      return aCoef * prevDb + (1.f - aCoef) * targetDb;
    if (mReleaseMode == 1)
      return std::max(targetDb, prevDb - unifStepDb);
    return rCoef * prevDb + (1.f - rCoef) * targetDb;
  }

  void ResetDisplay() {
    mBand = {};
    mCorrRing.fill(HopSum{});
    mCorrHead = 0;
    mSumLR = mSumL2 = mSumR2 = 0.0;
    mCorrValid = false;
    mCorrDisp = mWidthDisp = mBalDisp = 0.f;
    SetDirty(false);
  }

  // ── 几何 ──────────────────────────────────────────────────────────────
  // 画布: 标题行以下全部
  IRECT Canvas() const { return IRECT(mRECT.L, mRECT.T + kHeadH, mRECT.R, mRECT.B); }

  // 扇形几何: 圆心在画布底部中央 (基线上方留 L/R 标注带), 半径取高/宽较窄者
  void FanGeom(const IRECT &cv, float &cx, float &cy, float &rMax) const {
    cx = cv.L + cv.W() * 0.5f;
    cy = cv.B - kBasePad;
    rMax = std::max(std::min(cv.W() * 0.5f - kDomePad, (cy - cv.T) - kDomePad), 12.f);
  }

  // 显示 dB → 半径比例 (全扇同一刻度): db=0 → 外环, db=底限 → 圆心。
  float RadiusFor(float db, float rMax) const {
    return rMax * std::clamp((db - mFloorDb) / (0.f - mFloorDb), 0.f, 1.f);
  }

  // ── 绘制 ──────────────────────────────────────────────────────────────
  // 静态极坐标网格离屏 Layer: 只依赖范围底限与主题三值, 任一变化才重建 (与 pad 同策略)
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

  void DrawGridContent(IGraphics &g, const IRECT &cv) {
    float cx, cy, rMax;
    FanGeom(cv, cx, cy, rMax);
    IColor cL, cR, cM;
    GetChannelColors(cL, cR, cM);

    // 基线 (半扇直径; 两端 = 硬反相方位)
    g.DrawLine(COL_500(), cx - rMax - 10.f, cy, cx + rMax + 10.f, cy, nullptr, 1.f);

    // 反相红区背景 (PAZ 手册: >60° 两侧区域为红色): 基线两端 ±60°..±90°
    // 楔形 —— 侧翼/反相射线落在红区内, 视觉上属于显示区而非"跑出扇外"
    const IColor redZone(14, MeterRed().R, MeterRed().G, MeterRed().B);
    for (int k = 0; k < 2; ++k) {
      const float th0 = (k ? 1.f : -1.f) * (1.f / 3.f) * PI; // ±60°
      const float th1 = (k ? 1.f : -1.f) * 0.5f * PI;         // ±90° (基线端)
      g.PathClear();
      g.PathMoveTo(cx, cy);
      constexpr int kSegs = 16;
      for (int i = 0; i <= kSegs; ++i) {
        const float th = th0 + (th1 - th0) * (float)i / (float)kSegs;
        g.PathLineTo(cx + std::sin(th) * rMax, cy - std::cos(th) * rMax);
      }
      g.PathLineTo(cx, cy);
      g.PathClose();
      g.PathFill(IPattern(redZone));
    }

    // dB 同心圆环 (全扇上半圆弧, 多段子路径一次描边)
    g.PathClear();
    const int floorInt = (int)mFloorDb;
    for (int db = 0; db >= floorInt; db -= 20) {
      const float r = rMax * (float)(db - floorInt) / (float)-floorInt;
      if (r > 2.f)
        AddArcPath(g, cx, cy, r);
    }
    g.PathStroke(IPattern(WarmGray(135)), 1.f);

    // 刻度文字 (基线两端交点上方居中; 底限环 r=0 只留中央一个)
    const IText t(14, COL_700(), kFontRegular, EAlign::Center, EVAlign::Bottom);
    for (int db = 0; db >= floorInt; db -= 20) {
      const float r = rMax * (float)(db - floorInt) / (float)-floorInt;
      char buf[8];
      std::snprintf(buf, sizeof(buf), "%d", db);
      if (r > 20.f) {
        g.DrawText(t, buf, IRECT(cx - r - 20.f, cy - 17.f, cx - r + 20.f, cy - 2.f));
        g.DrawText(t, buf, IRECT(cx + r - 20.f, cy - 17.f, cx + r + 20.f, cy - 2.f));
      } else {
        g.DrawText(t, buf, IRECT(cx - 24.f, cy - 17.f, cx + 24.f, cy - 2.f));
      }
    }

    // ±45° 辐条: 同相 (中央 90°) 与左/右反相 (侧翼各 45°) 的分界
    for (int k = 0; k < 2; ++k) {
      const float th = (k ? 1.f : -1.f) * 0.25f * PI;
      g.DrawLine(WarmGray(165), cx, cy, cx + std::sin(th) * rMax, cy - std::cos(th) * rMax, nullptr, 1.f);
    }

    // 反相角标 (侧翼楔形内, 红字; 硬反相在基线两端)
    if (mAntiLabel) {
      const IText aT(11, MeterRed(), kFontSemiBold, EAlign::Center, EVAlign::Middle);
      for (int k = 0; k < 2; ++k) {
        const float th = (k ? 1.f : -1.f) * 0.375f * PI; // 侧翼 45° 区间中点 ±67.5°
        const float lx = cx + std::sin(th) * (rMax * 0.62f);
        const float ly = cy - std::cos(th) * (rMax * 0.62f);
        g.DrawText(aT, mAntiLabel, IRECT(lx - 50.f, ly - 9.f, lx + 50.f, ly + 9.f));
      }
    }

    // L / R 标注 (基线两端下方, 通道色, 与图例色块呼应)
    const IText lT(14, cL, kFontSemiBold, EAlign::Center, EVAlign::Top);
    const IText rT(14, cR, kFontSemiBold, EAlign::Center, EVAlign::Top);
    g.DrawText(lT, "L", IRECT(cx - rMax - 28.f, cy + 2.f, cx - rMax - 8.f, cy + 18.f));
    g.DrawText(rT, "R", IRECT(cx + rMax + 8.f, cy + 2.f, cx + rMax + 28.f, cy + 18.f));
  }

  // 上半圆弧路径子段 (方位角 -90°..+90° 采样折线逼近, 多段子路径共存一条路径)
  static void AddArcPath(IGraphics &g, float cx, float cy, float r) {
    constexpr int kSegs = 48;
    for (int i = 0; i <= kSegs; ++i) {
      const float th = (-0.5f + (float)i / kSegs) * PI;
      const float x = cx + std::sin(th) * r;
      const float y = cy - std::cos(th) * r;
      if (i == 0)
        g.PathMoveTo(x, y);
      else
        g.PathLineTo(x, y);
    }
  }

  // PAZ 式矢量线: 每带一条主射线 + 去相关/反相的 ±90° 侧翼射线
  void DrawVectors(IGraphics &g, const IRECT &cv) {
    float cx, cy, rMax;
    FanGeom(cv, cx, cy, rMax);
    IColor cL, cR, cM;
    GetChannelColors(cL, cR, cM);
    const IColor mainCol(235, cM.R, cM.G, cM.B);
    const IColor wingCol(140, cM.R, cM.G, cM.B);
    const IColor antiCol(235, MeterRed().R, MeterRed().G, MeterRed().B);

    // 静音/阈值判定: 只画全局峰值带以下 35 dB 内的带 (滤掉窗旁瓣泄漏的短线,
    //   单音只留 1-2 条线, 宽带内容仍全部可见)
    float maxDb = mFloorDb;
    for (int b = 0; b < kScopeBands; ++b)
      maxDb = std::max(maxDb, mBand[b].db);
    const float thr = std::max(maxDb - 35.f, mFloorDb + 0.5f);
    if (maxDb <= mFloorDb + 0.5f)
      return; // 静音: 圆心无残迹

    for (int b = 0; b < kScopeBands; ++b) {
      const float db = mBand[b].db;
      if (db < thr)
        continue;
      // 角度: 反相带直跳 (当前帧方位, ±90° 帧间交替 → 时间上两侧都点亮),
      //   非反相带用平滑值。绘制前钳到半扇角域作双重防御。
      const float thRaw = mBand[b].curAng;
      const float thSmooth = mBand[b].ang;
      const float th = std::clamp(mBand[b].anti ? thRaw : thSmooth, -0.5f * (float)PI,
                                  0.5f * (float)PI);
      const IColor col = mBand[b].anti ? antiCol : mainCol;
      const float r = RadiusFor(db, rMax);
      g.DrawLine(col, cx, cy, cx + std::sin(th) * r, cy - std::cos(th) * r, nullptr, 2.f);
      // 侧翼 ±90°: 只属于非反相的去相关内容 (反相带的连续方位由主射线本身呈现)
      if (!mBand[b].anti) {
        const float rw = r * mBand[b].wing;
        if (rw > 1.5f) {
          g.DrawLine(wingCol, cx, cy, cx + rw, cy, nullptr, 1.f);
          g.DrawLine(wingCol, cx, cy, cx - rw, cy, nullptr, 1.f);
        }
      }
    }
  }

  // 右栏读数: 相关性 (读数 + 分段色条) / 宽度 / 平衡。滑窗静音时显示 "—"
  void DrawReadouts(IGraphics &g, const IRECT &cv) {
    float cx, cy, rMax;
    FanGeom(cv, cx, cy, rMax);
    const float x0 = cx + rMax + 24.f;
    const float x1 = cv.R - 2.f;
    if (x1 - x0 < 90.f)
      return; // 极窄窗口防御
    const IText lblT(10.5f, COL_500(), kFontRegular, EAlign::Near, EVAlign::Middle);
    const IText valT(16, mCorrValid ? COL_900() : COL_500(), kFontSemiBold, EAlign::Near, EVAlign::Middle);
    char buf[24];

    // 相关性: ≥+0.3 绿 / 0..+0.3 黄 / <0 红 (与电平表安全色惯例一致), 条中点 = 0
    float y = cv.T + 8.f;
    if (mCorrLabel)
      g.DrawText(lblT, mCorrLabel, IRECT(x0, y, x1, y + 14.f));
    y += 14.f;
    if (mCorrValid)
      std::snprintf(buf, sizeof(buf), "%+.2f", mCorrDisp);
    else
      std::snprintf(buf, sizeof(buf), "%s", "—");
    g.DrawText(valT, buf, IRECT(x0, y, x1, y + 22.f));
    y += 25.f;
    const float barW = std::min(x1 - x0, 120.f);
    const IRECT bar(x0, y, x0 + barW, y + 5.f);
    g.FillRect(COL_300(), bar);
    if (mCorrValid) {
      const float mid = bar.L + barW * 0.5f;
      const float vx = mid + std::clamp(mCorrDisp, -1.f, 1.f) * (barW * 0.5f - 1.f);
      const IColor cc = (mCorrDisp >= 0.3f) ? MeterGreen() : (mCorrDisp >= 0.f) ? MeterYellow() : MeterRed();
      g.FillRect(cc, IRECT(std::min(mid, vx), bar.T, std::max(mid, vx), bar.B));
      g.FillRect(COL_700(), IRECT(mid - 0.5f, bar.T - 1.f, mid + 0.5f, bar.B + 1.f));
    }
    y += 20.f;

    // 宽度 (S/M 能量比): 0 dB = 单声道; 深负值显示 MONO
    if (mWidthLabel)
      g.DrawText(lblT, mWidthLabel, IRECT(x0, y, x1, y + 14.f));
    y += 14.f;
    if (mCorrValid) {
      if (mWidthDisp <= -35.f)
        std::snprintf(buf, sizeof(buf), "%s", "MONO");
      else
        std::snprintf(buf, sizeof(buf), "%+.1f dB", mWidthDisp);
    } else
      std::snprintf(buf, sizeof(buf), "%s", "—");
    g.DrawText(valT, buf, IRECT(x0, y, x1, y + 22.f));
    y += 36.f;

    // 平衡 (R/L 能量比): |bal| < 0.1 dB 显示 C
    if (mBalanceLabel)
      g.DrawText(lblT, mBalanceLabel, IRECT(x0, y, x1, y + 14.f));
    y += 14.f;
    if (mCorrValid) {
      if (std::fabs(mBalDisp) < 0.1f)
        std::snprintf(buf, sizeof(buf), "%s", "C");
      else
        std::snprintf(buf, sizeof(buf), "%s %.1f dB", (mBalDisp > 0.f) ? "R" : "L", std::fabs(mBalDisp));
    } else
      std::snprintf(buf, sizeof(buf), "%s", "—");
    g.DrawText(valT, buf, IRECT(x0, y, x1, y + 22.f));
  }

  // 1024 点基 2 FFT (迭代, bit 反转 + 蝶形), 正变换无缩放; UI 线程每 hop 一次
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

  // ── 状态 ──────────────────────────────────────────────────────────────
  struct Band {
    float db = -120.f;    // 主射线电平 (显示域弹道 dB)
    float ang = 0.f;      // 非反相带平滑方位 (弧度, 域被 kAntiThresh 约束在 ±56° 内)
    float curAng = 0.f;   // 当前帧方位 (反相带直跳用, 显示半扇角)
    float wing = 0.f;     // ±90° 侧翼长度比例 (0..1, 平滑; 仅非反相带绘制)
    bool anti = false;    // 反相带: 主射线红色, 角度直跳覆盖 ±45°..±90° 连续域
  };
  std::array<Band, kScopeBands> mBand{};

  struct HopSum {
    double lr, l2, r2;
  };
  std::array<HopSum, kCorrWin> mCorrRing{}; // 相关性滑窗 (每 hop 标量和)
  int mCorrHead = 0;
  double mSumLR = 0.0, mSumL2 = 0.0, mSumR2 = 0.0; // 滑窗内累计
  bool mCorrValid = false;                         // 窗内有能量 (静音显示 "—")
  float mCorrDisp = 0.f, mWidthDisp = 0.f, mBalDisp = 0.f; // 显示平滑值

  double mSampleRate = 48000.0;
  float mAttackSec = 0.05f;          // 接收保留 (电平攻击固定 10ms, 此值仅存档)
  float mReleaseSec = 0.2f;          // 释放时间常数 (LOG/LIN 档位, 与频谱同源)
  int mReleaseMode = 0;              // 0=LOG 对数域, 1=LIN 匀速
  float mFloorDb = -80.f;            // 半径 dB 底限 (外环 = 0 dB)
  float mHoldSec = 2.f;              // 峰值保持时长 (s, 接口保留, 矢量制式不绘制)

  const char *mTitle = nullptr, *mCorrLabel = nullptr, *mWidthLabel = nullptr;
  const char *mBalanceLabel = nullptr, *mAntiLabel = nullptr;

  // 静态网格离屏缓存; 状态哨兵初值保证首帧重建
  ILayerPtr mGridLayer;
  float mGridFloor = -1000.f;
  int mGridHue = -1;
  int mGridSat = -1;
  int mGridMode = -1;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE