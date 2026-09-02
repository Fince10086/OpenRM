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
  // 与 StereoScope 引擎的数据包严格一致 (ISender 数据包整体拷贝, 不一致会整包读取失败)
  using TDataPacket = std::array<float, 1024>;

  enum MsgTags {
    kMsgTagSampleRate = 1,
    kMsgTagRelease,
    kMsgTagReleaseMode,
    kMsgTagRange, // 显示 dB 底限 (float, 负值)
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
    } else if (msgTag == kMsgTagRelease) {
      float v;
      stream.Get(&v, 0);
      mReleaseSec = std::clamp(v, 0.01f, 10.f); // LIN 最慢档释放达 9.6s
    } else if (msgTag == kMsgTagReleaseMode) {
      int v;
      stream.Get(&v, 0);
      mReleaseMode = std::clamp(v, 0, 1);
    } else if (msgTag == kMsgTagRange) {
      float v;
      stream.Get(&v, 0);
      mFloorDb = std::clamp(v, -120.f, -30.f);
    } else if (msgTag == kMsgTagReset) {
      ResetDisplay();
    }
  }

  // hover 指针: 记录位置并请求重绘 (仅显示, 不捕获鼠标)
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
    // hover 读数标签先算好, 供刻度避让
    IRECT skipRect, labelBox;
    const bool hov = ComputeHover(g, mRECT, labelBox);
    skipRect = hov ? labelBox : IRECT();
    DrawGridLayer(g, mRECT);
    DrawTicks(g, mRECT, skipRect);
    DrawVectors(g, mRECT);
    DrawGauges(g, mRECT);
    if (hov)
      DrawHover(g, mRECT, labelBox);
  }

private:
  // ── 常量 ──────────────────────────────────────────────────────────────
  static constexpr int kFftN = 1024;          // hop = 1024 样本, 每包一次 FFT
  static constexpr int kScopeBands = 16;      // 对数频带数 (≈0.56 倍频程/带, 见 bin 表)
  static constexpr int kCorrWin = 64;         // 相关性滑窗 hop 数 (~1.4s @48k, 2 的幂)
  static constexpr float kFftAttackSec = 0.003f; // 电平攻击 (基本直跳, PAZ Peak 响度观感)
  static constexpr float kAntiWinFrames = 4.f;   // 反相权重窗帧数 (固定短窗, 闪频 ≈ PAZ)
  static constexpr int kImgBins = 64;            // 方位直方图 bin 数 (≈2.8°/bin)
  static inline const float kImgStep = (float)PI / (float)kImgBins; // 直方图角步进 (PI 非 constexpr, 故用 inline const)
  static constexpr float kImgKernelBins = 1.f;   // 注入核半宽 (bin)
  static constexpr float kImgDecaySec = 0.7f;    // 直方图时间衰减 (s, 射线持留)
  static constexpr float kImgDomeFrac = 0.10f;   // 穹顶半径占 rMax 比例
  static constexpr float kImgAntiPow = 2.f;      // 反相注入权重指数
  static constexpr float kImgWinFrames = 8.f;    // 同相组方位窗帧数 (中心锥宽)
  static constexpr double kFftCal = 0.375 * (double)kFftN * (double)kFftN; // 满幅正弦 = 0 dB 的 FFT 标定

  static constexpr float kBaseGap = 76.f;     // 圆心距画布底缘 (基线下方留白 → 仪表行)
  static constexpr float kRimLabelH = 19.f;   // 扇形顶部留白
  static constexpr float kGaugeRowH = 48.f;   // 仪表行高
  static constexpr float kGaugeW = 118.f;     // 单个仪表宽
  static constexpr float kGaugeGap = 4.f;     // 仪表间距
  static constexpr float kCxN = 0.28f;        // 圆心 x = 画布宽 × 0.28 (右侧留白)

  // 16 个对数频带的分 bin 表 (FFT 1024 @48k, bin ≈ 46.9 Hz; 逐带 ×1.477 ≈ 0.56 倍频程,
  // 47 Hz..23.9 kHz, 足以分开相差一个倍频程的成分, 对齐 PAZ 分带观感)。
  static constexpr int kBandBins[kScopeBands][2] = {
      {1, 1},   {2, 2},   {3, 3},   {4, 4},   {5, 7},   {8, 10},   {11, 15},  {16, 22},
      {23, 33}, {34, 49}, {50, 72}, {73, 107}, {108, 158}, {159, 234}, {235, 345}, {346, 510}};

  // ── 数据处理 ──────────────────────────────────────────────────────────
  void ProcessPacket(const ISenderData<2, TDataPacket> &d) {
    const double hopSec = 1024.0 / std::max(mSampleRate, 1.0);
    const float aCoef = (float)std::exp(-hopSec / kFftAttackSec);
    const float rCoef = (float)std::exp(-hopSec / std::max(mReleaseSec, 1e-3f));
    const float unifStep = (float)(hopSec / (2.0 * std::max(mReleaseSec, 1e-3f)) * -mFloorDb);

    // L/R 1024 点 FFT
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

    // 分带帧级聚合: E = Σ(|L|²+|R|²), DL = Σ(|R|²−|L|²), RC = ΣRe(L·conj R);
    // 每 bin 按 rck 符号分组: ≥0 同相组 (含静音侧 bin → 硬 pan 有确定方位), <0 反相组
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
    // 全带量 = 两组合成 (每 bin 必落一组)
    double eB[kScopeBands], dlB[kScopeBands], rcB[kScopeBands];
    for (int b = 0; b < kScopeBands; ++b) {
      eB[b] = eInB[b] + eAntiB[b];
      dlB[b] = dlInB[b] + dlAntiB[b];
      rcB[b] = rcInB[b] + rcAntiB[b];
    }

    // 相关性/宽度/平衡: kCorrWin hop 滑窗 (全带时域) + 150ms 显示平滑
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

    const float imgCoef = (float)std::exp(-hopSec / kImgDecaySec);
    for (int i = 0; i < kImgBins; ++i)
      mImg[i] *= imgCoef;
    auto injectLvl = [&](float th, float db) {
      if (db <= mFloorDb + 0.5f)
        return;
      const float rNorm = RadiusFor(db, 1.f); // 0..1 归一化电平
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
      // 同相组: 窗内方位直接注入 (噪声相位被窗口平均掉 → 中心锥; 相干内容方位不受窗影响)
      if (eInB[b] > 1e-12)
        injectLvl(groupTh((float)mDlInWin[b], (float)mRcInWin[b]),
                  orm::FastPwrToDb((float)(eInB[b] / kFftCal), -120.f));
      // 反相组: 窗内相关越负权重越大 (指数 kImgAntiPow), 仅电平衰减, 方位不变
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
    mCorrDisp = mWidthDisp = mBalDisp = 0.f;
    SetDirty(false);
  }

  // ── 几何 ──────────────────────────────────────────────────────────────
  void FanGeom(const IRECT &cv, float &cx, float &cy, float &rMax) const {
    cx = cv.L + cv.W() * kCxN;
    cy = cv.B - kBaseGap;
    rMax = std::max((cy - cv.T) - kRimLabelH, 12.f);
  }

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
  // 静态色块场离屏层: 底限与主题三值任一变化才重建 (刻度文字动态绘制, 见 DrawTicks)
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

  // 色块场: 12 个 15° 扇区 × 每 20 dB 一环; 亮度 = 角度维 × dB 维平均:
  // dB 维外圈亮 (245) → 圆心暗 (172); 角度维同相区向中心渐亮 (205→235)、翼区压暗 (195);
  // ±45° 亮度台阶即反相区分界 (无线条/颜色标记, 黑白主题成立)。
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
  }

  // 扇形填充 (折线逼近弧: 内缘外扩 0.75px、角向外扩 ~0.17°, 消除相邻填充的抗锯齿接缝)
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

  // 场内标注 (动态绘制, 供 hover 读数避让): L/R 在 ±45° 射线内侧, Anti Phase 在 ±90°
  // 基线内侧两端, dB 刻度沿中轴竖排 (0/−20/−40/… 随 Range)。
  void DrawTicks(IGraphics &g, const IRECT &cv, const IRECT &skipRect) {
    float cx, cy, rMax;
    FanGeom(cv, cx, cy, rMax);

    auto draw = [&](const IText &it, const char *txt, const IRECT &box) {
      if (!skipRect.Empty() && box.Intersects(skipRect))
        return;
      g.DrawText(it, txt, box);
    };

    // L / R (弧缘内 14px, 通道色)
    IColor cL, cR, cM;
    GetChannelColors(cL, cR, cM);
    const float d45 = (rMax - 14.f) / (float)std::sqrt(2.f);
    draw(IText(14, cL, kFontSemiBold, EAlign::Center, EVAlign::Middle), "L",
         IRECT(cx - d45 - 18.f, cy - d45 - 9.f, cx - d45 + 18.f, cy - d45 + 9.f));
    draw(IText(14, cR, kFontSemiBold, EAlign::Center, EVAlign::Middle), "R",
         IRECT(cx + d45 - 18.f, cy - d45 - 9.f, cx + d45 + 18.f, cy - d45 + 9.f));

    // Anti Phase (两端对称绘制)
    const IText apT(14, COL_700(), kFontSemiBold, EAlign::Center, EVAlign::Middle);
    const float ax = rMax - 50.f;
    draw(apT, "Anti Phase", IRECT(cx - ax - 36.f, cy - 19.f, cx - ax + 36.f, cy - 5.f));
    draw(apT, "Anti Phase", IRECT(cx + ax - 36.f, cy - 19.f, cx + ax + 36.f, cy - 5.f));

    // dB 刻度 (沿中轴内侧; 底限环 r=0 不标)
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

  // 方位直方图: 每角度 bin 一个采样点连成单一闭合路径 —— 无楔形/折线, 结构上杜绝
  // 跨缝连线; 穹顶为分离针底座; 填充为锚定峰值半径的径向渐变。
  void DrawVectors(IGraphics &g, const IRECT &cv) {
    float cx, cy, rMax;
    FanGeom(cv, cx, cy, rMax);
    IColor cL, cR, cM;
    GetChannelColors(cL, cR, cM);

    float maxImg = 0.f;
    for (int i = 0; i < kImgBins; ++i)
      maxImg = std::max(maxImg, mImg[i]);
    if (maxImg <= 1e-3f)
      return; // 静音/直方图衰减尽 → 无残迹

    const float rDome = std::max(3.f, rMax * kImgDomeFrac);
    const float rPeak = std::max(maxImg * rMax, rDome + 1.f);

    // 径向渐变 (同频谱公式): alpha = 15 + 240·exp(-3.5(1-s)), 穹顶最透, 峰值半径实色
    IPattern fill = IPattern::CreateRadialGradient(cx, cy, rPeak);
    {
      constexpr int kN = 8;
      constexpr float kMinA = 15.f; // 同频谱 kGradientMinAlpha
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

  // ── 相关性 / 宽度 / 平衡 ─────────────────────────
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

    // 拼读数: 最近频带 (仅阈值以上, 容差 0.14 rad ≈ 8°)
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

    // 标签沿指针方向放弧缘外侧, 超出画布左右缘时平移回画布内
    const IText t(14, COL_700(), kFontRegular, EAlign::Center, EVAlign::Bottom);
    const float px = cx + (rMax + 18.f) * std::sin(mHoverTh);
    const float py = cy - (rMax + 18.f) * std::cos(mHoverTh);
    IRECT box(px - 60.f, py - 17.f, px + 60.f, py - 1.f);
    g.MeasureText(t, mHoverBuf, box); // box 缩为文字实际宽度 (保持中心)
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
    float db = -120.f;  // 带电平 (显示域弹道 dB)
    float ang = 0.f;    // 连续方位 (0.5·atan2 全带聚合, 弧度; hover 读数用)
  };
  std::array<Band, kScopeBands> mBand{};
  std::array<float, kImgBins> mImg{}; // 方位直方图 (0..1 归一化电平, max 包络 + 每 hop 衰减)

  struct HopSum {
    double lr, l2, r2;
  };
  std::array<HopSum, kCorrWin> mCorrRing{};       // 相关性滑窗 (每 hop 标量和)
  int mCorrHead = 0;
  double mSumLR = 0.0, mSumL2 = 0.0, mSumR2 = 0.0; // 滑窗内累计
  bool mCorrValid = false;                         // 窗内有能量 (静音显示 "—")
  float mCorrDisp = 0.f, mWidthDisp = 0.f, mBalDisp = 0.f; // 显示平滑值

  double mSampleRate = 48000.0;
  float mReleaseSec = 0.2f;      // 释放时间常数 (LOG/LIN 档位, 与频谱同源)
  int mReleaseMode = 0;          // 0=LOG 对数域, 1=LIN 匀速
  float mFloorDb = -80.f;        // 半径 dB 底限 (外缘 = 0 dB)

  std::array<double, kScopeBands> mRcWin{}, mEWin{};      // 反相权重窗: 聚合相关/能量单极点 (kAntiWinFrames 帧)
  std::array<double, kScopeBands> mDlInWin{}, mRcInWin{}; // 同相组方位窗: 组聚合单极点 (kImgWinFrames 帧)
  char mHoverBuf[64] = "";       // hover 读数串 (ComputeHover 生成)

  // hover 指针 (OnMouseOver/OnMouseOut 维护)
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
