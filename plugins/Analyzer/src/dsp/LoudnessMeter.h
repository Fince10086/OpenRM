#pragma once

// LoudnessMeter — 响度测量引擎 (ITU-R BS.1770-4 / EBU R128 / EBU Tech 3342)
//
// 测量量:
//   M  Momentary   400ms 滑动窗 (100ms 步进, 无门限), LUFS
//   S  Short-Term  3000ms 滑动窗 (100ms 步进, 无门限), LUFS
//   I  Integrated  400ms 窗双门限 (绝对 -70 LUFS + 相对 均值-10 LU), LUFS
//   LRA            EBU 3342: 3s 块分布 10%..95% 最近秩百分位差, LU (首个 3s 块后即显示;
//                  统计参考需 ≥ 60s, 插件惯例提前显示)
//   TP Max         真峰值锁存 dBTP (由调用方喂入 4x 过采样真峰值, 见 LevelMeter;
//                  本引擎不做重复的过采样计算)
//
// 信号链: L/R → K 加权 (BS.1770-4 附录 2 四阶 IIR, 系数与参考实现 libebur128 一致)
//         → 每声道平方累加 → 100ms 子块推进 (引擎内部凑块, 与宿主块长无关)
//
// 校准: L = -0.691 + 10·log10(Σ G_i·z_i), G_L = G_R = 1.0 (z_i = 通道均方能量)
//   —— 1kHz 正弦 0 dBFS ≈ -3.01 LUFS (标准定义; 离线测试用 H(e^jω) 精确校验)
//
// 实现要点 (与 libebur128 对齐):
//   · 门限块 = 400ms 窗 100ms 步进 (75% 重叠, BS.1770-4 2011 修订)
//   · I 的相对门限重扫为精确 O(N): N = 已存门限块数, 每 100ms 一次 (会话级成本可忽略)
//   · LRA 直方图 0.1 LU 分辨率; 百分位前先剔除低于 (均值-20 LU) 的块 (EBU 3342)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace iplug {

class LoudnessMeter {
public:
  struct Snapshot {
    float momentary = -120.f;  // M, LUFS
    float shortTerm = -120.f;  // S, LUFS
    float integrated = -120.f; // I, LUFS
    float range = 0.f;         // LRA, LU
    float lraMin = -120.f;     // LRA 下界 (10% 百分位 LUFS, EBU 3342); UI 用作 bracket 下臂 y
    float lraMax = -120.f;     // LRA 上界 (95% 百分位 LUFS, EBU 3342); UI 用作 bracket 上臂 y
    float tpMax = -120.f;      // dBTP
    bool iValid = false;       // I 已有有效值 (≥1 个通过门限的 400ms 窗)
    bool lraValid = false;     // LRA 可显示 (≥3s; 统计参考需 60s)
  };

  void SetSampleRate(double sr) {
    mSR = sr;
    KWeightCoeffs(sr, mB, mA);
    mKfL.b[0] = mB[0]; mKfL.b[1] = mB[1]; mKfL.b[2] = mB[2]; mKfL.b[3] = mB[3]; mKfL.b[4] = mB[4];
    mKfL.a[0] = mA[0]; mKfL.a[1] = mA[1]; mKfL.a[2] = mA[2]; mKfL.a[3] = mA[3]; mKfL.a[4] = mA[4];
    mKfR = mKfL;
    mSubLen = std::max(1, (int)std::lround(sr * 0.1)); // 100ms 子块
    Reset();
  }

  void Reset() {
    mKfL.Reset();
    mKfR.Reset();
    mAccL = mAccR = 0.0;
    mSubPos = 0;
    mHops = 0;
    mFilled = 0;
    mHead = 0;
    for (float &v : mRing)
      v = 0.f;
    mSumS = 0.0;
    mGateBlocks.clear();
    mGateSum = 0.0;
    for (uint32_t &v : mHist)
      v = 0;
    mCurM = mCurS = mCurI = -120.f;
    mCurLra = 0.f;
    mCurLraMin = mCurLraMax = -120.f;
    mTpMax = -120.f;
    mIValid = false;
    mLraValid = false;
  }

  // 音频线程逐块喂入 (L/R 为宿主原始输入, 立体声权重各 1.0)
  void Process(const float *L, const float *R, int n) {
    for (int i = 0; i < n; ++i) {
      const double l = mKfL.Process(L[i]);
      const double r = mKfR.Process(R[i]);
      mAccL += l * l;
      mAccR += r * r;
      if (++mSubPos >= mSubLen) {
        mSubPos = 0;
        FlushSub();
        mAccL = mAccR = 0.0; // 子块能量已在 FlushSub 消费, 清零开始下一子块
      }
    }
  }

  // 喂入当前真峰值 (L/R, dBTP): 锁存历史最大 (复用 LevelMeter 的 4x 过采样结果)
  void SetTruePeaks(float tpL, float tpR) { mTpMax = std::max(mTpMax, std::max(tpL, tpR)); }

  void Store(Snapshot &s) const {
    s.momentary = mCurM;
    s.shortTerm = mCurS;
    s.integrated = mCurI;
    s.range = mCurLra;
    s.lraMin = mCurLraMin;
    s.lraMax = mCurLraMax;
    s.tpMax = mTpMax;
    s.iValid = mIValid;
    s.lraValid = mLraValid;
  }

  // K 加权四阶 IIR 系数 (BS.1770-4 附录 2; 与 libebur128 ebur128_init_filter 相同):
  // 级联 [1681.974Hz 架形双二阶 (含 +4dB @HF)] × [(1-z⁻¹)² / 38.135Hz 二阶高通]。
  // 注意: 第二级是有名数 0.5003270373238773 的纯高通 (分子 (1-z⁻¹)² 并入 b 多项式),
  // 不要按"高架+高通"的定性描述自行设计。
  static void KWeightCoeffs(double sr, double b[5], double a[5]) {
    const double pi = 3.14159265358979323846;
    // 级 1 双二阶
    const double f0 = 1681.974450955533;
    const double G = 3.999843853973347;
    const double Q = 0.7071752369554196;
    const double K = std::tan(pi * f0 / sr);
    const double Vh = std::pow(10.0, G / 20.0);
    const double Vb = std::pow(Vh, 0.4996667741545416);
    const double a0 = 1.0 + K / Q + K * K;
    const double pb0 = (Vh + Vb * K / Q + K * K) / a0;
    const double pb1 = 2.0 * (K * K - Vh) / a0;
    const double pb2 = (Vh - Vb * K / Q + K * K) / a0;
    const double pa1 = 2.0 * (K * K - 1.0) / a0;
    const double pa2 = (1.0 - K / Q + K * K) / a0;
    // 级 2 (仅分母; 分子 (1-z⁻¹)² 见下方 rb)
    const double f1 = 38.13547087602444;
    const double Q1 = 0.5003270373238773;
    const double K1 = std::tan(pi * f1 / sr);
    const double q0 = 1.0 + K1 / Q1 + K1 * K1;
    const double ra1 = 2.0 * (K1 * K1 - 1.0) / q0;
    const double ra2 = (1.0 - K1 / Q1 + K1 * K1) / q0;
    // 多项式卷积: b = pb ⊗ [1,-2,1]; a = pa ⊗ [1,ra1,ra2]
    b[0] = pb0;
    b[1] = -2.0 * pb0 + pb1;
    b[2] = pb0 - 2.0 * pb1 + pb2;
    b[3] = pb1 - 2.0 * pb2;
    b[4] = pb2;
    a[0] = 1.0;
    a[1] = ra1 + pa1;
    a[2] = ra2 + pa1 * ra1 + pa2;
    a[3] = pa1 * ra2 + pa2 * ra1;
    a[4] = pa2 * ra2;
  }

private:
  // 四阶直 II 型滤波器 (与 libebur128 同构; 双二阶级联展开成单段, 状态 4 个)
  struct KFilter {
    double b[5] = {1, 0, 0, 0, 0};
    double a[5] = {1, 0, 0, 0, 0};
    double v[4] = {0, 0, 0, 0};
    double Process(double x) {
      const double w = x - a[1] * v[0] - a[2] * v[1] - a[3] * v[2] - a[4] * v[3];
      const double y = b[0] * w + b[1] * v[0] + b[2] * v[1] + b[3] * v[2] + b[4] * v[3];
      v[3] = v[2];
      v[2] = v[1];
      v[1] = v[0];
      v[0] = w;
      return y;
    }
    void Reset() { v[0] = v[1] = v[2] = v[3] = 0.0; }
  };

  static constexpr int kMomentaryBlocks = 4; // 400ms
  static constexpr int kShortBlocks = 30;    // 3000ms
  static constexpr int kRingSize = kShortBlocks;
  static constexpr int kLraMinHops = 30;                 // 3s: 首个 3s 块后即开始显示 (60s 起才有统计意义)
  static constexpr int kLraBins = 1200;                  // LUFS -120..0, 0.1 LU 步进
  static constexpr double kAbsGateZ = 1.172443196018975e-7; // 10^((-70+0.691)/10), 绝对门限
  static constexpr double kRelGateFactor = 0.1;          // I 相对门限: 均值 -10 LU
  static constexpr double kLraGateFactor = 0.01;         // LRA 相对门限: 均值 -20 LU (EBU 3342)

  static float LoudnessFromEnergy(double z) {
    if (z <= 1e-12)
      return -120.f;
    return (float)std::max(-120.0, -0.691 + 10.0 * std::log10(z));
  }

  static int BinOf(float lufs) {
    return std::clamp((int)std::lround((lufs + 120.0) * 10.0), 0, kLraBins - 1);
  }

  // 每 100ms 子块边界: 更新 M/S, I 门限块, LRA 直方图
  void FlushSub() {
    const double zSub = (mAccL + mAccR) / (double)mSubLen; // 100ms 双声道加权能量 (G=1)
    const double old = mRing[mHead];                       // 即将被覆盖的旧值 (前 30 块为 0)
    mRing[mHead] = (float)zSub;
    mHead = (mHead + 1) % kRingSize;
    if (mFilled < kRingSize)
      ++mFilled;
    mSumS += zSub - old; // 运行和: 最近 30 子块
    ++mHops;

    // 最近 400ms 窗能量 (M 与 I 门限块同源)
    double sumM = 0.0;
    for (int i = 0; i < kMomentaryBlocks; ++i)
      sumM += mRing[(mHead - 1 - i + kRingSize) % kRingSize];

    // M (最近 400ms) / S (最近 3000ms)
    if (mFilled >= kMomentaryBlocks)
      mCurM = LoudnessFromEnergy(sumM / (double)kMomentaryBlocks);
    if (mFilled >= kShortBlocks)
      mCurS = LoudnessFromEnergy(mSumS / (double)kShortBlocks);

    // I: 400ms 窗双门限 (窗与 M 同源: 100ms 步进, 75% 重叠)
    if (mFilled >= kMomentaryBlocks) {
      const double z400 = sumM / (double)kMomentaryBlocks; // 400ms 窗平均能量
      if (z400 >= kAbsGateZ) {                             // 绝对门限 -70 LUFS
        mGateBlocks.push_back(z400);
        mGateSum += z400;
      }
      if (!mGateBlocks.empty()) {
        // 相对门限重扫 (精确): 阈值 = 绝对门限通过块均值 - 10 LU
        const double relThr = kRelGateFactor * (mGateSum / (double)mGateBlocks.size());
        double sumKeep = 0.0;
        uint64_t nKeep = 0;
        for (const double z : mGateBlocks)
          if (z >= relThr) {
            sumKeep += z;
            ++nKeep;
          }
        if (nKeep > 0) {
          mCurI = LoudnessFromEnergy(sumKeep / (double)nKeep);
          mIValid = true;
        } else {
          mCurI = -120.f; // 全部块被相对门限剔除 (病态节目), 与参考实现一致
          mIValid = false;
        }
      }
    }

    // LRA: 3s 块直方图 (仅收录 ≥ 绝对门限的块)
    if (mFilled >= kShortBlocks) {
      const double zS = mSumS / (double)kShortBlocks;
      if (zS >= kAbsGateZ)
        ++mHist[BinOf(LoudnessFromEnergy(zS))];
    }
    if (mHops >= kLraMinHops) {
      auto r = LraFromHist();
      mCurLra = r.range;
      mCurLraMin = r.lraMin;
      mCurLraMax = r.lraMax;
      mLraValid = true;
    }
  }

  // EBU 3342: 剔除低于 (均值-20 LU) 的 3s 块后, 取 10%..95% 最近秩百分位差
  // 同时暴露端点 lraMin/lraMax (10%/95% 百分位的 LUFS 值) 供 UI 用作
  // bracket 上下臂的 y 坐标映射 (见 SpectrumPad::DrawLraBracket)。
  struct LraResult {
    float range;  // LU (hBin - lBin) * 0.1
    float lraMin; // LUFS (10% 百分位, BinOf 反函数: bin/10 - 120)
    float lraMax; // LUFS (95% 百分位)
  };
  LraResult LraFromHist() const {
    static const std::array<double, kLraBins> kEnergy = [] {
      std::array<double, kLraBins> e{};
      for (int i = 0; i < kLraBins; ++i)
        e[i] = std::pow(10.0, (-120.0 + 0.1 * (double)i + 0.691) / 10.0);
      return e;
    }();
    double meanP = 0.0;
    uint64_t n = 0;
    for (int i = 0; i < kLraBins; ++i) {
      meanP += (double)mHist[i] * kEnergy[i];
      n += mHist[i];
    }
    if (n == 0)
      return {0.f, -120.f, -120.f};
    meanP /= (double)n;
    const double relThr = kLraGateFactor * meanP;
    uint64_t kept = 0;
    for (int i = 0; i < kLraBins; ++i)
      if (kEnergy[i] >= relThr)
        kept += mHist[i];
    if (kept <= 1)
      return {0.f, -120.f, -120.f};
    const double p10 = (double)(kept - 1) * 0.10 + 0.5;
    const double p95 = (double)(kept - 1) * 0.95 + 0.5;
    uint64_t cum = 0;
    int lBin = -1, hBin = -1;
    for (int i = 0; i < kLraBins; ++i) {
      if (kEnergy[i] < relThr)
        continue;
      cum += mHist[i];
      if (lBin < 0 && (double)cum > p10)
        lBin = i;
      if (hBin < 0 && (double)cum > p95) {
        hBin = i;
        break;
      }
    }
    if (lBin < 0 || hBin < 0)
      return {0.f, -120.f, -120.f};
    return {(float)((hBin - lBin) * 0.1), (float)lBin * 0.1f - 120.f, (float)hBin * 0.1f - 120.f};
  }

  double mSR = 48000.0;
  KFilter mKfL, mKfR;
  double mB[5] = {1, 0, 0, 0, 0};
  double mA[5] = {1, 0, 0, 0, 0};
  double mAccL = 0.0, mAccR = 0.0;
  int mSubLen = 4800, mSubPos = 0;
  uint64_t mHops = 0;
  float mRing[kRingSize] = {};
  int mHead = 0, mFilled = 0;
  double mSumS = 0.0;
  std::vector<double> mGateBlocks; // 通过绝对门限的 400ms 窗能量 (会话内累计, RESET 清空)
  double mGateSum = 0.0;
  std::array<uint32_t, kLraBins> mHist{};
  float mCurM = -120.f, mCurS = -120.f, mCurI = -120.f, mCurLra = 0.f, mTpMax = -120.f;
  float mCurLraMin = -120.f, mCurLraMax = -120.f; // LRA 直方图 10%/95% 百分位 LUFS 端点 (UI bracket 用)
  bool mIValid = false, mLraValid = false;
};

} // namespace iplug