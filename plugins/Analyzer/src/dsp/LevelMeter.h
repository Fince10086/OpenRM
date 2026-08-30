#pragma once

// LevelMeter — 专业级电平测量引擎 (L/R)
//
// L/R 条模式 (Process 的 mode 参数):
//   0: dBTP  真峰值 (ITU-R BS.1770-4: 4x 过采样后取峰值; EBU R128 建议上限 -1 dBTP)
//   1: dBFS + RMS (峰值瞬时 attack + 20 dB/s 线性回落; RMS 300ms 泄漏积分, IEC 60268-10)
//
// 独立 VU 表 (常驻, 与模式无关): IEC 60268-17: |x| 整流 -> 二阶临界阻尼积分,
// 99% 响应约 300ms, 0 VU = -18 dBFS; 峰值保持独立于 L/R 条 (mVuHold 对)。
//
// 附加: 峰值保持 (hold, 时长可调, 超时后 20 dB/s 衰减) 与过载锁存 (over latch, 手动清除)。

#include "FastMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace iplug {

class LevelMeter {
public:
  // 音频线程快照 (dB 域; holdDb 在 hold 关闭时为 -1000, 表示无效)
  struct Snapshot {
    float peakDbL = -120.f, peakDbR = -120.f;   // 样本峰值 (平滑后)
    float trueDbL = -120.f, trueDbR = -120.f;   // 真峰值 dBTP (平滑后)
    float rmsDbL = -120.f, rmsDbR = -120.f;     // RMS (300ms 积分)
    float vuDbL = -120.f, vuDbR = -120.f;       // VU 对应的 dBFS (0 VU = -18 dBFS)
    float vuHoldDbL = -120.f, vuHoldDbR = -120.f; // 独立 VU 表峰值保持 (显示域 dB)
    float persistDbL = -120.f, persistDbR = -120.f; // dBTP 持久锁存 (真峰值 > 0, 只增不减)
    float holdDbL = -120.f, holdDbR = -120.f;   // L/R 条峰值保持 (当前模式主值, 显示域 dB)
    float holdSec = 2.f;                        // 当前保持时长 (UI 端判断是否画 hold 线)
    int overL = 0, overR = 0;                   // 过载锁存
  };

  void SetSampleRate(double sr) {
    mSR = sr;
    const int ring = std::max(64, (int)std::lround(0.3 * sr)); // 300ms 滑动窗
    mRmsRingL.assign(ring, 0.f);
    mRmsRingR.assign(ring, 0.f);
    mRmsHeadL = mRmsHeadR = 0;
    mRmsSumL = mRmsSumR = 0.0;
    Reset();
  }

  void Reset() {
    mHistL.fill(0.f);
    mHistR.fill(0.f);
    mHistIdxL = mHistIdxR = 0;
    for (float &v : mRmsRingL)
      v = 0.f;
    for (float &v : mRmsRingR)
      v = 0.f;
    mRmsHeadL = mRmsHeadR = 0;
    mRmsSumL = mRmsSumR = 0.0;
    mVuPosL = mVuVelL = 0.0;
    mVuPosR = mVuVelR = 0.0;
    mVuHoldL = mVuHoldR = -120.f;
    mVuHoldTL = mVuHoldTR = 0.0;
    mPersistL = mPersistR = -120.f;
    mDispPeakL = mDispPeakR = -120.f;
    mDispTrueL = mDispTrueR = -120.f;
    mHoldL = mHoldR = -120.f;
    mHoldSec = 0.0;
    mOverL = mOverR = 0;
  }

  // 仅清除峰值保持与过载锁存 (RESET 按钮 / 模式切换)
  void ResetHoldOver() {
    mHoldL = mHoldR = -120.f;
    mVuHoldL = mVuHoldR = -120.f;
    mPersistL = mPersistR = -120.f;
    mOverL = mOverR = 0;
  }

  // 仅清除峰值保持 (模式切换时调用, 过载锁存保留)
  void ResetHold() {
    mHoldL = mHoldR = -120.f;
    mHoldTL = mHoldTR = 0.0;
    mVuHoldL = mVuHoldR = -120.f;
    mVuHoldTL = mVuHoldTR = 0.0;
  }

  // 仅清除 dBTP 真峰值持久锁存 (点击 dBTP 电平条)
  void ResetPersist() { mPersistL = mPersistR = -120.f; }

  // 仅清除 L/R 条峰值保持 (dBFS 模式下点击条体; VU 表保持不动)
  void ResetMeterHold() {
    mHoldL = mHoldR = -120.f;
    mHoldTL = mHoldTR = 0.0;
  }

  // 仅清除过载锁存 (dBFS 模式下点击顶部 LED)
  void ResetOver() { mOverL = mOverR = 0; }

  void Process(const float *L, const float *R, int n, int mode, double holdSec) {
    if (n <= 0)
      return;
    const double sr = (mSR > 0.0) ? mSR : 48000.0;
    mHoldSec = holdSec;

    const double dt = 1.0 / sr;
    const double w = 22.0; // VU 固有角频率: 临界阻尼二阶, 99% 响应 ≈ 300ms (IEC 60268-17)
    const double w2 = w * w;
    const double w2dt = w2 * dt;
    const double wdt2 = 2.0 * w * dt;

    float rawPeakL = 0.f, rawPeakR = 0.f;
    float tpPeakL = 0.f, tpPeakR = 0.f;
    switch (mode) {
    case 0:
      ProcessMode<0>(L, R, n, dt, w2dt, wdt2, rawPeakL, rawPeakR, tpPeakL, tpPeakR);
      break;
    default:
      ProcessMode<1>(L, R, n, dt, w2dt, wdt2, rawPeakL, rawPeakR, tpPeakL, tpPeakR);
      break;
    }

    const float peakDbL = AmpToDb(rawPeakL);
    const float peakDbR = AmpToDb(rawPeakR);
    const float trueDbL = AmpToDb(tpPeakL);
    const float trueDbR = AmpToDb(tpPeakR);
    const float rmsDbL = Db10(mRmsSumL / (double)mRmsRingL.size());
    const float rmsDbR = Db10(mRmsSumR / (double)mRmsRingR.size());
    const float vuDbL = VUToDb((float)mVuPosL);
    const float vuDbR = VUToDb((float)mVuPosR);

    // 显示 ballistics: 瞬时 attack + 20 dB/s 线性回落
    const float rate = 20.f * (float)n / (float)sr;
    mDispPeakL = Ball(mDispPeakL, peakDbL, rate);
    mDispPeakR = Ball(mDispPeakR, peakDbR, rate);
    mDispTrueL = Ball(mDispTrueL, trueDbL, rate);
    mDispTrueR = Ball(mDispTrueR, trueDbR, rate);

    // dBTP 真峰值持久锁存 (额外保持线): 峰值越过 0 dBFS 后只增不减, 直到 RESET
    if (mode == 0) {
      if (mDispTrueL > 0.f)
        mPersistL = std::max(mPersistL, mDispTrueL);
      if (mDispTrueR > 0.f)
        mPersistR = std::max(mPersistR, mDispTrueR);
    }

    // 过载锁存 (用瞬时值判定, L/R 条): dBTP > -1 / dBFS > 0
    const float overLv = (mode == 0) ? trueDbL : peakDbL;
    const float overRv = (mode == 0) ? trueDbR : peakDbR;
    const float overThr = (mode == 0) ? -1.f : 0.f;
    if (overLv > overThr)
      mOverL = 1;
    if (overRv > overThr)
      mOverR = 1;

    // L/R 条峰值保持 (基于当前模式主值)
    const float mainL = (mode == 0) ? mDispTrueL : mDispPeakL;
    const float mainR = (mode == 0) ? mDispTrueR : mDispPeakR;
    // 独立 VU 表峰值保持 (自己的状态, 与模式无关)
    const float dtBlock = (float)n / (float)sr;
    if (holdSec > 0.0) {
      mHoldL = HoldStep(mHoldL, mainL, dtBlock, rate, holdSec, mHoldTL);
      mHoldR = HoldStep(mHoldR, mainR, dtBlock, rate, holdSec, mHoldTR);
      mVuHoldL = HoldStep(mVuHoldL, vuDbL, dtBlock, rate, holdSec, mVuHoldTL);
      mVuHoldR = HoldStep(mVuHoldR, vuDbR, dtBlock, rate, holdSec, mVuHoldTR);
    } else {
      mHoldL = mHoldR = -1000.f;       // 无效, UI 不画 L/R 条 hold 线
      mVuHoldL = mVuHoldR = -1000.f;   // 无效, UI 不画 VU 表 hold 线
    }
  }

  void Store(Snapshot &s) const {
    s.peakDbL = mDispPeakL;
    s.peakDbR = mDispPeakR;
    s.trueDbL = mDispTrueL;
    s.trueDbR = mDispTrueR;
    s.rmsDbL = Db10(mRmsSumL / (double)mRmsRingL.size());
    s.rmsDbR = Db10(mRmsSumR / (double)mRmsRingR.size());
    s.vuDbL = VUToDb((float)mVuPosL);
    s.vuDbR = VUToDb((float)mVuPosR);
    s.vuHoldDbL = mVuHoldL;
    s.vuHoldDbR = mVuHoldR;
    s.persistDbL = mPersistL;
    s.persistDbR = mPersistR;
    s.holdDbL = mHoldL;
    s.holdDbR = mHoldR;
    s.holdSec = mHoldSec;
    s.overL = mOverL;
    s.overR = mOverR;
  }

private:
  static constexpr int kPhaseTaps = 12; // 每相位抽头数 (总 4 × 12 = 48 阶)
  static constexpr int kHistCap = 16;   // 真峰值历史环形容量 (2 的幂, ≥ 抽头数-1)
  static constexpr int kHistMask = kHistCap - 1;

  // 逐样本循环按模式模板分派 (0=dBTP, 1=dBFS+RMS): 真峰值/RMS 只按当前模式执行,
  // 模板常量折叠消除每样本分支; rawPeak 全模式收集, 保持切换模式时 peak ballistics 连续。
  // VU 积分器全模式无条件运行: UI 的独立 VU 表常驻显示功率 (0 VU = -18 dBFS), 与电平模式无关。
  template <int MODE>
  void ProcessMode(const float *L, const float *R, int n, double dt, double w2dt, double wdt2,
                   float &rawPeakL, float &rawPeakR, float &tpPeakL, float &tpPeakR) {
    const int ringSize = (int)mRmsRingL.size();
    for (int i = 0; i < n; ++i) {
      const float l = L[i], r = R[i];
      rawPeakL = std::max(rawPeakL, std::fabs(l));
      rawPeakR = std::max(rawPeakR, std::fabs(r));
      StepVu(mVuPosL, mVuVelL, std::fabs(l), dt, w2dt, wdt2);
      StepVu(mVuPosR, mVuVelR, std::fabs(r), dt, w2dt, wdt2);
      if (MODE == 0) {
        tpPeakL = std::max(tpPeakL, TruePeakSample(l, mHistL, mHistIdxL));
        tpPeakR = std::max(tpPeakR, TruePeakSample(r, mHistR, mHistIdxR));
      } else if (MODE == 1) {
        // RMS 300ms 滑动窗口 (IEC 60268-10 短积分行为, 无泄漏积分收敛误差)
        mRmsSumL += (double)l * l - mRmsRingL[mRmsHeadL];
        mRmsRingL[mRmsHeadL] = l * l;
        mRmsHeadL = (mRmsHeadL + 1) % ringSize;
        mRmsSumR += (double)r * r - mRmsRingR[mRmsHeadR];
        mRmsRingR[mRmsHeadR] = r * r;
        mRmsHeadR = (mRmsHeadR + 1) % ringSize;
      }
    }
  }

  // 4x 半带插值 FIR 系数 (Blackman 窗 sinc, 各相位直流增益归一化到 1)。
  // 存储为 [tap][phase] 主序: 同一历史样本一次读取喂满 4 个相位累加器, 利于 SIMD。
  static const std::array<std::array<float, 4>, kPhaseTaps> &PolyCoeffs() {
    static const auto poly = [] {
      constexpr int L = 4 * kPhaseTaps;
      constexpr float kPi = 3.14159265358979323846f;
      const float c = (float)(L - 1) * 0.5f;
      std::array<std::array<float, 4>, kPhaseTaps> p{};
      for (int n = 0; n < L; ++n) {
        const float x = ((float)n - c) / 4.f;
        float h = (std::fabs(x) < 1e-6f) ? 4.f : 4.f * std::sin(kPi * x) / (kPi * x);
        const float w = 0.42f - 0.5f * std::cos(2.f * kPi * (float)n / (float)(L - 1)) +
                        0.08f * std::cos(4.f * kPi * (float)n / (float)(L - 1));
        p[n / 4][n % 4] = h * w;
      }
      double g[4] = {0.0, 0.0, 0.0, 0.0};
      for (int j = 0; j < kPhaseTaps; ++j)
        for (int k = 0; k < 4; ++k)
          g[k] += p[j][k];
      for (int j = 0; j < kPhaseTaps; ++j)
        for (int k = 0; k < 4; ++k)
          if (std::fabs(g[k]) > 1e-9)
            p[j][k] /= (float)g[k];
      return p;
    }();
    return poly;
  }

  // 单样本 4x 过采样: 返回该样本 4 个插值输出的最大绝对值, 并推进环形历史。
  // 历史为环形缓冲 (容量 16, 掩码寻址), 消除逐样本整体移位拷贝。
  static float TruePeakSample(float x, std::array<float, kHistCap> &hist, int &idx) {
    hist[idx] = x;
    idx = (idx + 1) & kHistMask;
    const auto &poly = PolyCoeffs();
    float y0 = 0.f, y1 = 0.f, y2 = 0.f, y3 = 0.f;
    for (int j = 0; j < kPhaseTaps; ++j) {
      const float h = hist[(idx - 1 - j) & kHistMask];
      y0 += poly[j][0] * h;
      y1 += poly[j][1] * h;
      y2 += poly[j][2] * h;
      y3 += poly[j][3] * h;
    }
    return std::max(std::max(std::fabs(y0), std::fabs(y1)), std::max(std::fabs(y2), std::fabs(y3)));
  }

  // VU 二阶临界阻尼积分 (symplectic Euler): 99% 阶跃响应 ≈ 300ms
  static void StepVu(double &pos, double &vel, float u, double dt, double w2dt, double wdt2) {
    vel += w2dt * ((double)u - pos) - wdt2 * vel;
    pos += vel * dt;
  }

  static float Ball(float disp, float raw, float rate) {
    if (raw > disp)
      return raw;
    return std::max(raw, disp - rate);
  }

  static float HoldStep(float hold, float main, float dtBlock, float rate, double holdSec, double &holdT) {
    if (main > hold) {
      hold = main;
      holdT = 0.0;
    } else {
      holdT += dtBlock;
      if (holdT >= holdSec)
        hold = std::max(main, hold - rate);
    }
    return hold;
  }

  static float AmpToDb(float a) { return orm::FastAmpToDb(a, -120.f); }
  static float Db10(double v) { return orm::FastPwrToDb((float)v, -120.f); }
  // VU 位置 (平均整流值) -> 正弦 RMS dBFS: RMS = avg * π/(2√2) ≈ 1.1107 * avg
  static float VUToDb(float avg) { return (avg > 1e-8f) ? orm::FastAmpToDb(1.1107f * avg, -120.f) : -120.f; }

  double mSR = 48000.0;
  std::array<float, kHistCap> mHistL{}, mHistR{}; // 真峰值 4x 插值历史 (环形)
  int mHistIdxL = 0, mHistIdxR = 0;               // 环形写指针
  std::vector<float> mRmsRingL, mRmsRingR; // 300ms 能量滑动窗
  int mRmsHeadL = 0, mRmsHeadR = 0;
  double mRmsSumL = 0.0, mRmsSumR = 0.0;
  double mVuPosL = 0.0, mVuVelL = 0.0;
  double mVuPosR = 0.0, mVuVelR = 0.0;
  float mDispPeakL = -120.f, mDispPeakR = -120.f;
  float mDispTrueL = -120.f, mDispTrueR = -120.f;
  float mHoldL = -120.f, mHoldR = -120.f;
  double mHoldSec = 0.0;
  double mHoldTL = 0.0, mHoldTR = 0.0;
  float mVuHoldL = -120.f, mVuHoldR = -120.f; // 独立 VU 表峰值保持 (与 L/R 条 mHold 独立)
  double mVuHoldTL = 0.0, mVuHoldTR = 0.0;
  float mPersistL = -120.f, mPersistR = -120.f; // dBTP 持久锁存 (真峰值 > 0, 只增不减)
  int mOverL = 0, mOverR = 0;
};

} // namespace iplug
