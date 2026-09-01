#pragma once

// StereoFieldControl — 立体声声像显示 (PAZ Position 式极坐标矢量线)
//
// 位于频谱下方 (布局 20,336 → 752,604)。数据管线与频谱同构:
// StereoScope 引擎在音频线程只攒 1024 样本 hop 原始包 → ISender 队列 →
// OnIdle TransmitData → 本控件 OnMsgFromDelegate; FFT/分带/弹道全部在 UI
// 线程完成 (重活不过音频线程)。
//
// 显示制式对齐 Waves PAZ Position (SPD): 极坐标矢量图 —— 每个显示元素是
// 一条从圆心发出的射线, 长度 = 响度 (dB 半径刻度), 角度 = 声像位置; 显示
// 角度 = 模型方位角折半 (δ=θ/2): 同相 |θ|≤90° → 中央 ±45°, 反相 → 侧翼
// 45°..90°, 硬反相 → 基线两端, 整幅正好 180° 半扇。
//
// 视觉制式 (与频谱/功能键区同一套设计语言):
//   * 无标题行、无任何说明性小字 —— 只有刻度文字 (14px) 与读数 (16px);
//   * 无装饰细线: 环/辐条/基线一律不描线, 背景为频谱同款的极坐标色块场
//     (12 个 15° 角扇区 × 每 20 dB 一圈环带, 亮度 = 角度维 × dB 维平均后过
//     WarmGray, 黑白主题自动退成纯灰阶); ±45° 处的亮度台阶就是反相区分界,
//     不用红色 (MeterRed 固定 RGB 在黑白主题下穿帮);
//   * 射线为 2px 实心内容线 (与电平保持线同地位): 同相带 = M 通道色,
//     反相带 = COL_900 极墨 (两种主题下都是最重的一笔), ±90° 侧翼杆 =
//     2px COL_700 实心短杆;
//   * 相关性/宽度/平衡三联仪表行位于基线下方: 16px 读数 + 6px 轨道 +
//     2px 指针 + 14px 刻度, 无词标, 语义色段走电平条的 satScale 处理;
//   * hover 径向指针 (唯一允许的 1px 线) + 角度/频带/电平复合读数。
//
// 每 hop 对 L/R 做 1024 点 FFT, 按 16 个对数频带聚合 (Σ|L|², Σ|R|²,
// ΣRe(L·conj R)) → 每带一条主射线 (帧级方位 + 电平弹道)。反相带 (相关
// < −0.2) 的主射线角度 = 帧级 (平衡, 相关) 的自然半角映射, 连续覆盖
// ±45°..±90° 整个侧翼, 硬反相时 ±90° 在帧间交替点亮两侧; 非反相的去相关
// 内容在 ±90° 补两条侧翼杆。电平弹道攻击基本直跳 (3ms), 释放沿用 LOG/LIN
// 档位; 主射线角度 30ms 平滑、侧翼比例 100ms 平滑。HOLD 在矢量制式下不再
// 有视觉, 接口保留; RESET 清空显示状态。

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

  // hover 十字指针: IGraphics 对悬停控件每次鼠标移动都会回调 OnMouseOver,
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
    const IRECT cv = mRECT;
    // hover 读数标签矩形先算好 (与刻度避让, 同频谱准线逻辑)
    IRECT skipRect, labelBox;
    const bool hov = ComputeHover(g, cv, labelBox);
    skipRect = hov ? labelBox : IRECT();
    DrawGridLayer(g, cv);
    DrawTicks(g, cv, skipRect);
    DrawVectors(g, cv);
    DrawGauges(g, cv);
    if (hov)
      DrawHover(g, cv, labelBox);
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
  static constexpr float kBaseGap = 76.f;     // 圆心距画布底缘 (基线 → L/R 标 → 仪表行)
  static constexpr float kRimLabelH = 19.f;   // 弧外角度刻度行高 (扇顶之上留白)
  static constexpr float kGaugeRowH = 48.f;   // 仪表行高 (读数 18 + 轨道 6 + 刻度 16 + 边距)
  static constexpr float kGaugeW = 118.f;     // 单个仪表宽
  static constexpr float kGaugeGap = 4.f;     // 仪表间距 (与右栏按钮 kBtnGap 一致)
  static constexpr float kCxN = 0.28f;        // 圆心 x = 画布宽 × 0.28 (内容靠左, 右侧留白)

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
  // 画布 = 整个控件矩形 (无标题行); 内容靠左, 右侧留白
  IRECT Canvas() const { return mRECT; }

  // 扇形几何: 圆心距底缘 kBaseGap (基线下方依次为 L/R 标与仪表行),
  // 半径 = 圆心到弧外刻度行的距离
  void FanGeom(const IRECT &cv, float &cx, float &cy, float &rMax) const {
    cx = cv.L + cv.W() * kCxN;
    cy = cv.B - kBaseGap;
    rMax = std::max((cy - cv.T) - kRimLabelH, 12.f);
  }

  // 显示 dB → 半径 (全扇同一刻度): db=0 → 外缘, db=底限 → 圆心
  float RadiusFor(float db, float rMax) const {
    return rMax * std::clamp((db - mFloorDb) / (0.f - mFloorDb), 0.f, 1.f);
  }

  // ── 语义色 (固定 RGB 安全色随主题饱和档位降饱和, 黑白主题下变灰阶) ────
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

  // ── 绘制 ──────────────────────────────────────────────────────────────
  // 静态极坐标色块场离屏 Layer: 只依赖范围底限与主题三值, 任一变化才重建
  // (与频谱网格同策略); 刻度文字动态绘制 (供 hover 读数避让), 见 DrawTicks。
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

  // 极坐标色块场: 12 个 15° 角扇区 × 每 20 dB 一圈环带。
  // 每格亮度 = 角度维 × dB 维平均 (与频谱 DrawBackground 同常数同函数):
  //   dB 维: 外圈亮 (245) → 圆心暗 (172), 随 Range 行数变化;
  //   角度维: 同相区 (|θ|≤45°) 向中心渐亮 (205→235), 两侧翼区压暗一档 (195);
  //   ±45° 处的亮度台阶 = 反相区分界 (无线条、无颜色标记, 黑白主题成立)。
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
        const float vAng =
            (aa <= 0.25f * (float)PI) ? (235.f - 30.f * (aa / (0.25f * (float)PI))) : 195.f;
        const int v = (int)std::lround(0.5f * (vAng + vDb));
        FillSector(g, cx, cy, rLo, rHi, a0, a1, WarmGray(v));
      }
    }
    // L / R 标注 (基线两端下方, 通道色, 与图例色块呼应; 硬反相方位)
    IColor cL, cR, cM;
    GetChannelColors(cL, cR, cM);
    const IText lT(14, cL, kFontSemiBold, EAlign::Center, EVAlign::Top);
    const IText rT(14, cR, kFontSemiBold, EAlign::Center, EVAlign::Top);
    g.DrawText(lT, "L", IRECT(cx - rMax - 28.f, cy + 2.f, cx - rMax - 8.f, cy + 18.f));
    g.DrawText(rT, "R", IRECT(cx + rMax + 8.f, cy + 2.f, cx + rMax + 28.f, cy + 18.f));
  }

  // 环带扇形填充 (折线逼近弧, 3.75°/步 → 弦高 <0.1px 无锯齿感);
  // 内缘外扩 0.75px、两侧角向各外扩 ~0.17°, 消除相邻填充的抗锯齿接缝
  // (与频谱色块 1px 重叠同策略)。
  static void FillSector(IGraphics &g, float cx, float cy, float rLo, float rHi, float a0, float a1,
                         const IColor &c) {
    if (rHi - rLo <= 0.5f)
      return;
    constexpr float kPad = 0.003f;
    constexpr int kSegs = 4; // 15° / 3.75°
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

  // 刻度文字 (动态绘制, 供 hover 读数避让; 无任何刻度线):
  //   角度刻度在弧外 (−90/−45/0/+45/+90, 14px COL_700);
  //   dB 刻度沿中轴内侧竖排 (0/−20/−40/… 随 Range, 贴环下缘中轴右侧)。
  void DrawTicks(IGraphics &g, const IRECT &cv, const IRECT &skipRect) {
    float cx, cy, rMax;
    FanGeom(cv, cx, cy, rMax);
    const IText t(14, COL_700(), kFontRegular, EAlign::Center, EVAlign::Bottom);

    auto angleLabel = [&](float px, float py, const char *txt, const IRECT &box) {
      if (!skipRect.Empty() && box.Intersects(skipRect))
        return;
      g.DrawText(t, txt, box);
    };

    // 0°: 弧顶正上方
    angleLabel(cx, 0, "0", IRECT(cx - 22.f, cv.T + 1.f, cx + 22.f, cv.T + 17.f));
    // ±45°: 弧缘外 16px 处
    const float d45 = (rMax + 16.f) * (float)std::sin(0.25f * (float)PI);
    const float dy45 = (rMax + 16.f) * (float)std::cos(0.25f * (float)PI);
    angleLabel(cx - d45, cy - dy45, "-45", IRECT(cx - d45 - 20.f, cy - dy45 - 16.f, cx - d45 + 20.f, cy - dy45));
    angleLabel(cx + d45, cy - dy45, "45", IRECT(cx + d45 - 20.f, cy - dy45 - 16.f, cx + d45 + 20.f, cy - dy45));
    // ±90°: 基线两端上方
    angleLabel(0, 0, "-90", IRECT(cx - rMax - 40.f, cy - 17.f, cx - rMax - 6.f, cy - 1.f));
    angleLabel(0, 0, "90", IRECT(cx + rMax + 6.f, cy - 17.f, cx + rMax + 40.f, cy - 1.f));

    // dB 刻度 (沿中轴内侧, 标在环位下方; 底限环 r=0 不标)
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

  // PAZ 式矢量线: 每带一条主射线 + 去相关的 ±90° 侧翼杆 (2px 实心内容线)
  void DrawVectors(IGraphics &g, const IRECT &cv) {
    float cx, cy, rMax;
    FanGeom(cv, cx, cy, rMax);
    IColor cL, cR, cM;
    GetChannelColors(cL, cR, cM);
    const IColor mainCol(cM.R, cM.G, cM.B);
    const IColor wingCol = COL_700();
    const IColor antiCol = COL_900();

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
        if (rw > 2.f) {
          g.FillRect(wingCol, IRECT(cx - rw, cy - 1.f, cx - 1.f, cy + 1.f));
          g.FillRect(wingCol, IRECT(cx + 1.f, cy - 1.f, cx + rw, cy + 1.f));
        }
      }
    }
  }

  // ── 基线下仪表行 (无词标): 相关性 / 宽度 / 平衡 ─────────────────────────
  // 每项 = 16px 读数 + 6px 轨道 (COL_300) + 2px 指针 + 14px 刻度; 语义色段
  // 走 SemColor (黑白主题自动变灰阶)。
  void DrawGauges(IGraphics &g, const IRECT &cv) {
    const float y0 = cv.B - kGaugeRowH;
    const float trackT = y0 + 22.f, trackB = trackT + 6.f;
    const IText valT(16, COL_900(), kFontSemiBold, EAlign::Near, EVAlign::Middle);
    const IText dimT(16, COL_500(), kFontSemiBold, EAlign::Near, EVAlign::Middle);
    const IText tickT(14, COL_700(), kFontRegular, EAlign::Near, EVAlign::Top);
    IColor cL, cR, cM;
    GetChannelColors(cL, cR, cM);
    char buf[24];

    for (int gi = 0; gi < 3; ++gi) {
      const float x0 = cv.L + gi * (kGaugeW + kGaugeGap);
      const float x1 = x0 + kGaugeW;
      const IRECT cell(x0, y0, x1, cv.B);
      const IRECT track(x0, trackT, x1, trackB);
      g.FillRect(COL_300(), track);

      if (gi == 0) {
        // ── 相关性: −1..+1, 中点 = 0; ≥+0.3 绿 / 0..+0.3 黄 / <0 红 ──
        if (mCorrValid)
          std::snprintf(buf, sizeof(buf), "%+.2f", mCorrDisp);
        else
          std::snprintf(buf, sizeof(buf), "%s", "—");
        g.DrawText(mCorrValid ? valT : dimT, buf, cell);
        if (mCorrValid) {
          const IColor cc = (mCorrDisp >= 0.3f)   ? SemColor(MeterGreen())
                            : (mCorrDisp >= 0.f)  ? SemColor(MeterYellow())
                                                  : SemColor(MeterRed());
          const float mid = x0 + kGaugeW * 0.5f;
          const float vx = x0 + (std::clamp(mCorrDisp, -1.f, 1.f) + 1.f) * 0.5f * kGaugeW;
          g.FillRect(cc, IRECT(std::min(mid, vx), trackT, std::max(mid, vx), trackB));
          g.FillRect(cc, IRECT(vx - 1.f, trackT, vx + 1.f, trackB));
        }
        const IText lT(14, COL_700(), kFontRegular, EAlign::Near, EVAlign::Top);
        const IText cT(14, COL_700(), kFontRegular, EAlign::Center, EVAlign::Top);
        const IText rT(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Top);
        g.DrawText(lT, "-1", IRECT(x0, y0 + 30.f, x0 + 30.f, cv.B - 2.f));
        g.DrawText(cT, "0", IRECT(x0 + kGaugeW * 0.5f - 16.f, y0 + 30.f, x0 + kGaugeW * 0.5f + 16.f, cv.B - 2.f));
        g.DrawText(rT, "+1", IRECT(x1 - 30.f, y0 + 30.f, x1, cv.B - 2.f));
      } else if (gi == 1) {
        // ── 宽度 (S/M 能量比): 0 dB = 单声道 (右端), 左向加宽; 深负值 MONO ──
        if (mCorrValid) {
          if (mWidthDisp <= -35.f)
            std::snprintf(buf, sizeof(buf), "%s", "MONO");
          else
            std::snprintf(buf, sizeof(buf), "%+.1f dB", mWidthDisp);
        } else
          std::snprintf(buf, sizeof(buf), "%s", "—");
        g.DrawText(mCorrValid ? valT : dimT, buf, cell);
        if (mCorrValid) {
          const float frac = std::clamp(mWidthDisp, -24.f, 0.f) / -24.f;
          const float vx = x1 - frac * kGaugeW;
          g.FillRect(COL_900(), IRECT(vx - 1.f, trackT, vx + 1.f, trackB));
        }
        g.DrawText(tickT, "0", IRECT(x1 - 30.f, y0 + 30.f, x1, cv.B - 2.f));
        g.DrawText(IText(14, COL_700(), kFontRegular, EAlign::Center, EVAlign::Top), "-12",
                   IRECT(x0 + kGaugeW * 0.5f - 20.f, y0 + 30.f, x0 + kGaugeW * 0.5f + 20.f, cv.B - 2.f));
        g.DrawText(IText(14, COL_700(), kFontRegular, EAlign::Near, EVAlign::Top), "-24",
                   IRECT(x0, y0 + 30.f, x0 + 34.f, cv.B - 2.f));
      } else {
        // ── 平衡 (R/L 能量比): 中点 C; 指针用所在侧通道色 ──
        if (mCorrValid) {
          if (std::fabs(mBalDisp) < 0.1f)
            std::snprintf(buf, sizeof(buf), "%s", "C");
          else
            std::snprintf(buf, sizeof(buf), "%s %.1f dB", (mBalDisp > 0.f) ? "R" : "L",
                          std::fabs(mBalDisp));
        } else
          std::snprintf(buf, sizeof(buf), "%s", "—");
        g.DrawText(mCorrValid ? valT : dimT, buf, cell);
        if (mCorrValid) {
          const float mid = x0 + kGaugeW * 0.5f;
          const float vx = mid + std::clamp(mBalDisp, -12.f, 12.f) / 12.f * (kGaugeW * 0.5f);
          const IColor nc = (std::fabs(mBalDisp) < 0.1f) ? COL_900() : (mBalDisp > 0.f ? cR : cL);
          g.FillRect(nc, IRECT(std::min(mid, vx), trackT, std::max(mid, vx), trackB));
          g.FillRect(nc, IRECT(vx - 1.f, trackT, vx + 1.f, trackB));
        }
        g.DrawText(IText(14, IColor(255, cL.R, cL.G, cL.B), kFontRegular, EAlign::Near, EVAlign::Top), "L",
                   IRECT(x0, y0 + 30.f, x0 + 20.f, cv.B - 2.f));
        g.DrawText(IText(14, COL_700(), kFontRegular, EAlign::Center, EVAlign::Top), "C",
                   IRECT(x0 + kGaugeW * 0.5f - 12.f, y0 + 30.f, x0 + kGaugeW * 0.5f + 12.f, cv.B - 2.f));
        g.DrawText(IText(14, IColor(255, cR.R, cR.G, cR.B), kFontRegular, EAlign::Far, EVAlign::Top), "R",
                   IRECT(x1 - 20.f, y0 + 30.f, x1, cv.B - 2.f));
      }
    }
  }

  // ── hover 径向指针 + 复合读数 ────────────────────────────────────────────
  // 指针 = 唯一允许的 1px 线 (十字指针的径向形态): 圆心 → 弧缘穿过光标方向。
  // 读数 = 显示角 (°) + 最近频带中心频率 + 该带电平, 标签放弧缘外侧, 与固定
  // 刻度重叠时让位 (skipRect 由 Draw 传给 DrawTicks 隐藏被压住的刻度)。
  struct HoverInfo {
    bool active = false;
    float th = 0.f; // 指针方位角 (弧度, −π/2..+π/2, 0 = 正上)
  };

  bool ComputeHover(IGraphics &g, const IRECT &cv, IRECT &labelBox) {
    labelBox = IRECT();
    if (!mHoverActive)
      return false;
    float cx, cy, rMax;
    FanGeom(cv, cx, cy, rMax);
    const float dx = mHoverX - cx, dyUp = cy - mHoverY;
    if (mHoverY > cy || dx * dx + dyUp * dyUp > rMax * rMax + 4.f)
      return false; // 光标不在半扇内
    mHoverTh = std::atan2(dx, std::max(dyUp, 0.001f));

    // 最近频带 (按绘制方位角; 仅统计阈值以上的带)
    float maxDb = mFloorDb;
    for (int b = 0; b < kScopeBands; ++b)
      maxDb = std::max(maxDb, mBand[b].db);
    const float thr = std::max(maxDb - 35.f, mFloorDb + 0.5f);
    float best = 1e9f;
    int bi = -1;
    for (int b = 0; b < kScopeBands; ++b) {
      if (mBand[b].db < thr)
        continue;
      const float th = std::clamp(mBand[b].anti ? mBand[b].curAng : mBand[b].ang,
                                  -0.5f * (float)PI, 0.5f * (float)PI);
      const float d = std::fabs(th - mHoverTh);
      if (d < best) {
        best = d;
        bi = b;
      }
    }

    char buf[64];
    if (bi >= 0 && best < 0.14f) { // 命中容差 ~8°
      const float hz = (float)(0.5 * (kBandBins[bi][0] + kBandBins[bi][1])) * mSampleRate / kFftN;
      char fb[16];
      if (hz < 1000.f)
        std::snprintf(fb, sizeof(fb), "%.0f Hz", hz);
      else
        std::snprintf(fb, sizeof(fb), "%.2f kHz", hz / 1000.f);
      std::snprintf(buf, sizeof(buf), "%.1f°  %s  %.1f dB", mHoverTh * 180.f / (float)PI, fb,
                    mBand[bi].db);
    } else {
      std::snprintf(buf, sizeof(buf), "%.1f°", mHoverTh * 180.f / (float)PI);
    }

    // 标签放弧缘外侧沿指针方向, 超出画布左右缘时平移回画布内
    const IText t(14, COL_700(), kFontRegular, EAlign::Center, EVAlign::Bottom);
    const float px = cx + (rMax + 18.f) * std::sin(mHoverTh);
    const float py = cy - (rMax + 18.f) * std::cos(mHoverTh);
    IRECT box(px - 60.f, py - 17.f, px + 60.f, py - 1.f);
    g.MeasureText(t, buf, box); // box 缩为文字实际宽度 (保持中心)
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
    // 复合读数文字 (框已算好)
    const IText t(14, COL_700(), kFontRegular, EAlign::Near, EVAlign::Bottom);
    char buf[64];
    float maxDb = mFloorDb;
    for (int b = 0; b < kScopeBands; ++b)
      maxDb = std::max(maxDb, mBand[b].db);
    const float thr = std::max(maxDb - 35.f, mFloorDb + 0.5f);
    float best = 1e9f;
    int bi = -1;
    for (int b = 0; b < kScopeBands; ++b) {
      if (mBand[b].db < thr)
        continue;
      const float th = std::clamp(mBand[b].anti ? mBand[b].curAng : mBand[b].ang,
                                  -0.5f * (float)PI, 0.5f * (float)PI);
      const float d = std::fabs(th - mHoverTh);
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
      std::snprintf(buf, sizeof(buf), "%.1f°  %s  %.1f dB", mHoverTh * 180.f / (float)PI, fb,
                    mBand[bi].db);
    } else {
      std::snprintf(buf, sizeof(buf), "%.1f°", mHoverTh * 180.f / (float)PI);
    }
    g.DrawText(t, buf, labelBox);
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
    bool anti = false;    // 反相带: 主射线极墨, 角度直跳覆盖 ±45°..±90° 连续域
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
  float mAttackSec = 0.05f;          // 接收保留 (电平攻击固定 3ms, 此值仅存档)
  float mReleaseSec = 0.2f;          // 释放时间常数 (LOG/LIN 档位, 与频谱同源)
  int mReleaseMode = 0;              // 0=LOG 对数域, 1=LIN 匀速
  float mFloorDb = -80.f;            // 半径 dB 底限 (外缘 = 0 dB)
  float mHoldSec = 2.f;              // 峰值保持时长 (s, 接口保留, 矢量制式不绘制)

  // hover 十字指针 (OnMouseOver/OnMouseOut 维护)
  bool mHoverActive = false;
  float mHoverX = 0.f, mHoverY = 0.f;
  float mHoverTh = 0.f;

  // 静态网格离屏缓存; 状态哨兵初值保证首帧重建
  ILayerPtr mGridLayer;
  float mGridFloor = -1000.f;
  int mGridHue = -1;
  int mGridSat = -1;
  int mGridMode = -1;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
