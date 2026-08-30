#pragma once

// StereoFieldControl — 立体声声像显示 (PAZ Position 式极坐标电平扇形)
//
// 位于频谱下方空闲区 (布局 20,336 → 752,604)。数据管线与频谱同构:
// StereoScope 引擎在音频线程只攒 1024 样本 hop 原始包 → ISender 队列 →
// OnIdle TransmitData → 本控件 OnMsgFromDelegate; 方位角/能量分桶/弹道/相关性
// 全部在 UI 线程完成 (重活不过音频线程, 与 SpectrumSTFT 的 UI 侧 FFT 同策略)。
//
// 模型 (逐样本): θ = atan2(R²−L², 2·L·R) —— 0 = 正上 = 居中, ±90° = 极右/极左,
// |θ| > 90° = 反相 (等价 2·atan2(S,M) 的方位角倍频, 振幅声像位置在上半圆线性摊开);
// 能量 E = L²+R² 按角度分 180 桶累积, 桶平均功率转 dB (满幅单声道正弦 = 0 dB 标定)
// 后逐桶走显示域弹道 —— 与频谱共用 Attack/Release/STD·MAX·AVG/LOG·LIN 参数。
// 包络画成自底部圆心张开的扇形: 上半区 (同相) 主题 M 色填充; 下半区 (反相) 半径
// 压缩到独立短带内、红色语义 —— PAZ 的反相区几乎不可见, 这里改为始终可见且不占
// 穹顶空间 (两种半径刻度, dB 圆环只对上半区有效)。
// HOLD 开启时叠加逐桶峰值轮廓 (刷新清计时, 超时 20 dB/s 回落, RESET 按钮清除;
// 计龄用音频时钟 hop 时长, 冻结回放按回放帧推进, 保持确定性语义)。
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
    kMsgTagBallistic,
    kMsgTagRange, // 显示 dB 底限 (float, 负值)
    kMsgTagHold,  // 峰值保持时长 (s, 0 = 关; 与电平表 hold 共用开关/档位)
    kMsgTagReset, // 清空桶状态/峰值保持/相关性窗口 (配置重建/冻结回放前)
  };

  explicit StereoFieldControl(const IRECT &bounds) : IControl(bounds) {
    mDispDb.fill(mFloorDb);
    mHoldDb.fill(-1000.f);
    // 桶区间划分: [0, mMidLo) 左反相 / [mMidLo, mMidHi) 同相 / [mMidHi, kBins) 右反相
    for (int b = 0; b < kBins; ++b) {
      if (BinAngle(b) >= -0.5f * PI) {
        mMidLo = b;
        break;
      }
    }
    for (int b = mMidLo; b < kBins; ++b) {
      if (BinAngle(b) >= 0.5f * PI) {
        mMidHi = b;
        break;
      }
    }
    if (mMidHi <= mMidLo)
      mMidHi = kBins;
  }

  // RESET 按钮联动: 清除峰值保持轮廓
  void ClearPeakHold() {
    mHoldDb.fill(-1000.f);
    mHoldAge.fill(0.f);
    mHoldSignal = false;
    SetDirty(false);
  }

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
      mReleaseSec = std::clamp(v, 0.01f, 2.f);
    } else if (msgTag == kMsgTagReleaseMode) {
      int v;
      stream.Get(&v, 0);
      mReleaseMode = std::clamp(v, 0, 1);
    } else if (msgTag == kMsgTagBallistic) {
      int v;
      stream.Get(&v, 0);
      mBallistic = std::clamp(v, 0, 2);
    } else if (msgTag == kMsgTagRange) {
      float v;
      stream.Get(&v, 0);
      mFloorDb = std::clamp(v, -120.f, -30.f);
    } else if (msgTag == kMsgTagHold) {
      stream.Get(&mHoldSec, 0);
      if (mHoldSec <= 0.f) {
        if (mHoldWasActive) {
          ClearPeakHold();
          mHoldWasActive = false;
        }
      } else {
        mHoldWasActive = true;
      }
    } else if (msgTag == kMsgTagReset) {
      ResetDisplay();
    }
  }

  // hover 读数: 光标处方位角 + 半径对应 dB (IControl 每次鼠标移动回调)
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
    const IRECT cv = Canvas();
    DrawGridLayer(g, cv);
    DrawEnvelope(g, cv);
    DrawReadouts(g, cv);
    DrawHover(g, cv);
  }

private:
  // ── 常量 ──────────────────────────────────────────────────────────────
  static constexpr int kBins = 180;          // 全圆角度桶数 (2°/桶, PAZ 式棱角分辨率)
  static constexpr int kCorrWin = 64;        // 相关性滑窗 hop 数 (~1.4s @48k, 2 的幂)
  static constexpr float kHeadH = 22.f;      // 顶部标题行高
  static constexpr float kAntiBandH = 34.f;  // 反相区 (水平轴以下) 高度
  static constexpr float kDomePad = 8.f;     // 穹顶距画布上/左右边缘留白
  static constexpr float kHoldFallDb = 20.f; // 峰值保持超时回落速率 (dB/s, 与频谱一致)
  static constexpr float kAvgTauSec = 0.1f;  // AVG 档平均时间常数 (与频谱一致)
  static constexpr int kHoldLineAlpha = 75;  // 峰值保持轮廓不透明度 (与频谱 hold 线一致)

  // ── 数据处理 ──────────────────────────────────────────────────────────
  // 每 hop 包: 方位角分桶 + 相关性滑窗 + 逐桶显示域弹道 + 峰值保持
  void ProcessPacket(const ISenderData<2, TDataPacket> &d) {
    const double hopSec = 1024.0 / std::max(mSampleRate, 1.0);
    // 弹道系数每包按帧进给周期重算 (与 pad 同式, 时间常数与引擎同步)
    mAttackCoeff = (float)std::exp(-hopSec / std::max(mAttackSec, 1e-3f));
    mReleaseCoeff = (float)std::exp(-hopSec / std::max(mReleaseSec, 1e-3f));
    // 匀速档: 每帧固定"半径满幅比例"下落 (2·τ 秒跨全幅, 恒像素速度)
    mUnifStepDb = (float)(hopSec / (2.0 * std::max(mReleaseSec, 1e-3f)) * -mFloorDb);
    mAvgAlpha = (float)(1.0 - std::exp(-hopSec / kAvgTauSec));

    // 逐样本: θ = atan2(R²−L², 2·L·R) 分桶 + 相关性滑窗标量
    mBinE.fill(0.0);
    double lr = 0.0, l2 = 0.0, r2 = 0.0;
    const float *inL = d.vals[0].data();
    const float *inR = d.vals[1].data();
    for (int i = 0; i < 1024; ++i) {
      const double l = inL[i], r = inR[i];
      const double ll = l * l, rr = r * r;
      lr += l * r;
      l2 += ll;
      r2 += rr;
      const double e = ll + rr;
      if (e < 1e-12)
        continue; // -120 dBFS 以下不计 (静音不造方位)
      const int b = (int)((std::atan2(rr - ll, 2.0 * l * r) + PI) * (0.5 * kBins / PI));
      mBinE[b >= kBins ? kBins - 1 : b] += e;
    }

    // 相关性/宽度/平衡: kCorrWin hop 滑窗 (比值之和再作商, 等效 1.4s 线性积分) + 150ms 显示平滑
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

    // 逐桶显示域弹道 (与频谱同一弹道学: STD 攻放单极点 / MAX 瞬时上升 / AVG 功率平均)。
    // 桶平均功率按"满幅单声道正弦 = 0 dB"标定 (E/1024 = 逐样本平均功率)。
    const float over = 0.1f * -mFloorDb; // 屏底 overshoot 缓冲 (与频谱同式, 回落不拖尾)
    const bool doAvg = (mBallistic == 2);
    const bool instAttack = (mBallistic == 1);
    for (int b = 0; b < kBins; ++b) {
      float rawDb;
      if (doAvg) {
        float &p = mAvgP[b];
        p += mAvgAlpha * ((float)mBinE[b] * (1.f / 1024.f) - p);
        rawDb = orm::FastPwrToDb(p, -120.f);
      } else {
        rawDb = orm::FastPwrToDb((float)mBinE[b] * (1.f / 1024.f), -120.f);
      }
      const float target = std::clamp(rawDb, mFloorDb - over, 0.f);
      mDispDb[b] = doAvg ? target
                         : StepSmoothed(mDispDb[b], target, mAttackCoeff, mReleaseCoeff, mUnifStepDb,
                                        instAttack);
    }

    UpdatePeakHold(hopSec);
    SetDirty(false);
  }

  // 峰值保持 (与频谱 hold 曲线同规则): 刷新即清计时, 超时后 20 dB/s 回落, 下限为当前显示值。
  // 计龄用音频时钟 (hop 时长) 而非墙钟: 冻结回放时随回放帧推进, 与确定性回放语义一致。
  void UpdatePeakHold(double hopSec) {
    if (mHoldSec <= 0.f) {
      if (mHoldWasActive) {
        ClearPeakHold();
        mHoldWasActive = false;
      }
      return;
    }
    mHoldWasActive = true;
    const float fallDb = (mHoldSec < 1e8f) ? kHoldFallDb * (float)hopSec : 0.f; // ∞ 档恒不回落
    for (int b = 0; b < kBins; ++b) {
      const float cur = mDispDb[b];
      float &hold = mHoldDb[b];
      if (cur > hold) {
        hold = cur;
        mHoldAge[b] = 0.f;
        if (cur > mFloorDb + 0.5f)
          mHoldSignal = true;
      } else {
        mHoldAge[b] += (float)hopSec;
        if (mHoldAge[b] > mHoldSec)
          hold = std::max(cur, hold - fallDb);
      }
    }
  }

  // 单点弹道 (与 SpectrumPad::StepSmoothed 同式): 攻击 dB 域单极点; 回落按模式
  //   LOG = 显示域差距等比收缩, UNIF = 恒定屏幅比例速率; instAttack = 上升瞬时到位
  float StepSmoothed(float prevDb, float targetDb, float aCoef, float rCoef, float unifStepDb,
                     bool instAttack) const {
    if (targetDb > prevDb)
      return instAttack ? targetDb : aCoef * prevDb + (1.f - aCoef) * targetDb;
    if (mReleaseMode == 1)
      return std::max(targetDb, prevDb - unifStepDb);
    return rCoef * prevDb + (1.f - rCoef) * targetDb;
  }

  void ResetDisplay() {
    mDispDb.fill(mFloorDb);
    mHoldDb.fill(-1000.f);
    mHoldAge.fill(0.f);
    mAvgP.fill(0.f);
    mHoldSignal = false;
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

  // 扇形几何: 圆心在画布底部中央 (反相带顶边), 穹顶半径取高/宽较窄者
  void FanGeom(const IRECT &cv, float &cx, float &cy, float &rMax) const {
    cx = cv.L + cv.W() * 0.5f;
    cy = cv.B - kAntiBandH;
    rMax = std::max(std::min(cv.W() * 0.5f - kDomePad, (cy - cv.T) - kDomePad), 12.f);
  }

  // 桶 b 的中心方位角 (弧度): 0 = 正上 (居中), +π/2 = 极右, ±π = 正下 (反相)
  static float BinAngle(int b) { return -PI + (2.f * PI * (b + 0.5f)) / (float)kBins; }

  // 显示 dB → 半径比例: 同相区用穹顶全幅; 反相区 (|θ|>90°) 压缩到反相带内。
  // f: db=0 → 1 (外环), db=底限 → 0 (圆心)。
  float RadiusFor(float th, float db, float rMax) const {
    const float f = std::clamp((db - mFloorDb) / (0.f - mFloorDb), 0.f, 1.f);
    return (std::fabs(th) > 0.5f * PI ? (kAntiBandH - 6.f) : rMax) * f;
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

    // 反相带淡红染色 + 基线 (±90° = 极左/极右方向)
    g.FillRect(IColor(12, MeterRed().R, MeterRed().G, MeterRed().B), IRECT(cv.L, cy, cv.R, cv.B));
    g.DrawLine(COL_500(), cx - rMax - 10.f, cy, cx + rMax + 10.f, cy, nullptr, 1.f);

    // dB 同心圆环 (上半圆弧, 多段子路径一次描边)
    g.PathClear();
    const int floorInt = (int)mFloorDb;
    for (int db = 0; db >= floorInt; db -= 20) {
      const float r = rMax * (float)(db - floorInt) / (float)-floorInt;
      if (r > 2.f)
        AddArcPath(g, cx, cy, r);
    }
    g.PathStroke(IPattern(WarmGray(135)), 1.f);

    // 刻度文字 (基线上方, 环与基线交点居中; 底限环 r=0 只留中央一个)
    const IText t(14, COL_700(), kFontRegular, EAlign::Center, EVAlign::Bottom);
    for (int db = 0; db >= floorInt; db -= 20) {
      const float r = rMax * (float)(db - floorInt) / (float)-floorInt;
      char buf[8];
      std::snprintf(buf, sizeof(buf), "%d", db);
      if (r > 20.f) {
        g.DrawText(t, buf, IRECT(cx - r - 20.f, cy - 17.f, cx - r + 20.f, cy - 1.f));
        g.DrawText(t, buf, IRECT(cx + r - 20.f, cy - 17.f, cx + r + 20.f, cy - 1.f));
      } else {
        g.DrawText(t, buf, IRECT(cx - 24.f, cy - 17.f, cx + 24.f, cy - 1.f));
      }
    }

    // ±45° 辐条 (声像 ±50% 参考线)
    for (int k = 0; k < 2; ++k) {
      const float th = (k ? 1.f : -1.f) * 0.25f * PI;
      g.DrawLine(WarmGray(165), cx, cy, cx + std::sin(th) * rMax, cy - std::cos(th) * rMax, nullptr, 1.f);
    }

    // L / R 标注 (基线两端下方, 通道色, 与图例色块呼应)
    const IText lT(14, cL, kFontSemiBold, EAlign::Center, EVAlign::Top);
    const IText rT(14, cR, kFontSemiBold, EAlign::Center, EVAlign::Top);
    g.DrawText(lT, "L", IRECT(cx - rMax - 28.f, cy + 3.f, cx - rMax - 8.f, cy + 19.f));
    g.DrawText(rT, "R", IRECT(cx + rMax + 8.f, cy + 3.f, cx + rMax + 28.f, cy + 19.f));

    // 反相角标 (带内两角, 红字; 射线可达范围只到圆心 ±反相带宽, 角落恒为空)
    if (mAntiLabel) {
      const float bandMidY = cy + kAntiBandH * 0.5f;
      const IText aTL(11, MeterRed(), kFontSemiBold, EAlign::Near, EVAlign::Middle);
      g.DrawText(aTL, mAntiLabel, IRECT(cv.L + 4.f, bandMidY - 8.f, cx - rMax - 32.f, bandMidY + 8.f));
      const IText aTR(11, MeterRed(), kFontSemiBold, EAlign::Far, EVAlign::Middle);
      g.DrawText(aTR, mAntiLabel, IRECT(cx + rMax + 32.f, bandMidY - 8.f, cv.R - 4.f, bandMidY + 8.f));
    }
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

  void DrawEnvelope(IGraphics &g, const IRECT &cv) {
    float cx, cy, rMax;
    FanGeom(cv, cx, cy, rMax);
    IColor cL, cR, cM;
    GetChannelColors(cL, cR, cM);

    // 全空判定: 任一桶高于底限 +0.5 才绘制 (静音时圆心无残迹)
    bool any = false;
    for (int b = 0; b < kBins && !any; ++b)
      any = mDispDb[b] > mFloorDb + 0.5f;

    if (any) {
      // 同相扇形 (上半区, 主题 M 色: 填充 + 亮描边)
      BuildFanPath(g, cx, cy, rMax, mMidLo, mMidHi);
      g.PathFill(IPattern(IColor(105, cM.R, cM.G, cM.B)));
      g.PathStroke(IPattern(IColor(230, cM.R, cM.G, cM.B)), 1.f);
      // 反相扇形 (左右两段, 红色语义 = 朝轴下张开的反相能量)
      const IColor antiFill(80, MeterRed().R, MeterRed().G, MeterRed().B);
      const IColor antiLine(190, MeterRed().R, MeterRed().G, MeterRed().B);
      BuildFanPath(g, cx, cy, rMax, 0, mMidLo);
      g.PathFill(IPattern(antiFill));
      g.PathStroke(IPattern(antiLine), 1.f);
      BuildFanPath(g, cx, cy, rMax, mMidHi, kBins);
      g.PathFill(IPattern(antiFill));
      g.PathStroke(IPattern(antiLine), 1.f);
    }

    if (mHoldSec > 0.f && mHoldSignal)
      DrawHoldOutline(g, cx, cy, rMax);
  }

  // 扇形路径: 圆心 → 桶 [b0,b1) 中心角各一点 → 闭合 (星形多边形, 填充与描边共用)
  void BuildFanPath(IGraphics &g, float cx, float cy, float rMax, int b0, int b1) {
    g.PathClear();
    g.PathMoveTo(cx, cy);
    for (int b = b0; b < b1; ++b) {
      const float th = BinAngle(b);
      const float r = RadiusFor(th, mDispDb[b], rMax);
      g.PathLineTo(cx + std::sin(th) * r, cy - std::cos(th) * r);
    }
    g.PathClose();
  }

  // 峰值保持轮廓: 连续有效段折线 (半透明极细线, 与频谱 hold 曲线同色同规则)
  void DrawHoldOutline(IGraphics &g, float cx, float cy, float rMax) {
    const IColor col(kHoldLineAlpha, COL_900().R, COL_900().G, COL_900().B);
    g.PathClear();
    bool inRun = false;
    for (int b = 0; b < kBins; ++b) {
      if (mHoldDb[b] <= mFloorDb + 0.5f) {
        inRun = false;
        continue;
      }
      const float th = BinAngle(b);
      const float r = RadiusFor(th, mHoldDb[b], rMax);
      const float x = cx + std::sin(th) * r, y = cy - std::cos(th) * r;
      if (inRun)
        g.PathLineTo(x, y);
      else
        g.PathMoveTo(x, y);
      inRun = true;
    }
    g.PathStroke(IPattern(col), 1.f);
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

  // 悬停读数: 光标方位角 + 半径对应 dB (反相带内带 AP 前缀), 径向辅助线提示所读射线
  void DrawHover(IGraphics &g, const IRECT &cv) {
    if (!mHoverActive)
      return;
    float cx, cy, rMax;
    FanGeom(cv, cx, cy, rMax);
    const float dx = mHoverX - cx, dy = mHoverY - cy;
    const float r = std::sqrt(dx * dx + dy * dy);
    const float antiR = kAntiBandH - 6.f;
    float rRef;
    if (dy <= 0.f && r <= rMax + 2.f)
      rRef = rMax;
    else if (dy > 0.f && r <= antiR + 2.f)
      rRef = antiR;
    else
      return;
    const float db = mFloorDb * std::clamp(r / rRef, 0.f, 1.f);
    const float th = std::atan2(dx, -dy); // 0 = 正上, ±π (反相区 |θ| > 90°)
    const float deg = th * 180.f / PI;
    const float inv = (r > 0.5f) ? rRef / r : 0.f;
    g.DrawLine(IColor(70, COL_700().R, COL_700().G, COL_700().B), cx, cy, cx + dx * inv, cy + dy * inv,
               nullptr, 1.f);

    char buf[40];
    if (std::fabs(deg) > 90.f)
      std::snprintf(buf, sizeof(buf), "AP %s %.0f°  %.1f dB", (deg < 0.f) ? "L" : "R",
                    180.f - std::fabs(deg), db);
    else {
      const char *side = (deg > 1.f) ? "R" : (deg < -1.f) ? "L" : "C";
      std::snprintf(buf, sizeof(buf), "%s %.0f°  %.1f dB", side, std::fabs(deg), db);
    }
    // 标签在光标右上, 右缘/顶缘放不下时翻转
    const IText t(14, COL_700(), kFontRegular, EAlign::Near, EVAlign::Bottom);
    IRECT labelR(mHoverX + 8.f, mHoverY - 18.f, mRECT.R - 2.f, mHoverY - 2.f);
    g.MeasureText(t, buf, labelR);
    if (labelR.R > mRECT.R - 2.f)
      labelR = IRECT(mHoverX - 8.f - labelR.W(), mHoverY - 18.f, mHoverX - 8.f, mHoverY - 2.f);
    if (labelR.T < mRECT.T)
      labelR = IRECT(labelR.L, mHoverY + 4.f, labelR.L + labelR.W(), mHoverY + 20.f);
    g.DrawText(t, buf, labelR);
  }

  // ── 状态 ──────────────────────────────────────────────────────────────
  std::array<float, kBins> mDispDb{};  // 逐桶显示域弹道 dB
  std::array<float, kBins> mHoldDb{};  // 逐桶峰值保持 (显示 dB, -1000 = 无效)
  std::array<float, kBins> mHoldAge{}; // 逐桶距上次刷新峰值的时间 (音频时钟, s)
  std::array<float, kBins> mAvgP{};    // AVG 档逐桶功率平均状态
  std::array<double, kBins> mBinE{};   // 本包角度能量累积
  bool mHoldSignal = false;            // 已有有效峰值 (全无效不画轮廓)
  bool mHoldWasActive = false;         // hold 开关边沿检测 (关闭时清一次积累)

  struct HopSum {
    double lr, l2, r2;
  };
  std::array<HopSum, kCorrWin> mCorrRing{}; // 相关性滑窗 (每 hop 标量和)
  int mCorrHead = 0;
  double mSumLR = 0.0, mSumL2 = 0.0, mSumR2 = 0.0; // 滑窗内累计
  bool mCorrValid = false;                         // 窗内有能量 (静音显示 "—")
  float mCorrDisp = 0.f, mWidthDisp = 0.f, mBalDisp = 0.f; // 显示平滑值

  double mSampleRate = 48000.0;
  float mAttackSec = 0.05f, mReleaseSec = 0.2f; // 与频谱 Attack/Release 参数同源
  int mReleaseMode = 0;                         // 0=LOG 对数域, 1=LIN 匀速
  int mBallistic = 0;                           // 0=STD, 1=MAX, 2=AVG
  float mFloorDb = -80.f;                       // 半径 dB 底限 (外环 = 0 dB)
  float mHoldSec = 2.f;                         // 峰值保持时长 (s, 0 = 关)
  float mAttackCoeff = 0.2f, mReleaseCoeff = 0.9f, mUnifStepDb = 1.f, mAvgAlpha = 0.f;

  int mMidLo = 45, mMidHi = 135; // 桶区间: [0,mMidLo) 左反相 / [mMidLo,mMidHi) 同相 / [mMidHi,kBins) 右反相

  bool mHoverActive = false;
  float mHoverX = 0.f, mHoverY = 0.f;

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
