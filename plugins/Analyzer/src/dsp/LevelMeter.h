#pragma once

// LevelMeter — 专业级三模式电平测量引擎 (L/R)
//
// 模式:
//   0: dBTP  真峰值 (ITU-R BS.1770-4: 4x 过采样后取峰值; EBU R128 建议上限 -1 dBTP)
//   1: dBFS + RMS (峰值瞬时 attack + 20 dB/s 线性回落; RMS 300ms 泄漏积分, IEC 60268-10)
//   2: VU    (IEC 60268-17: |x| 整流 -> 二阶临界阻尼积分, 99% 响应约 300ms, 0 VU = -18 dBFS)
//
// 附加: 峰值保持 (hold, 时长可调, 超时后 20 dB/s 衰减) 与过载锁存 (over latch, 手动清除)。

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
    float holdDbL = -120.f, holdDbR = -120.f;   // 峰值保持 (显示域 dB)
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
    for (float &v : mRmsRingL)
      v = 0.f;
    for (float &v : mRmsRingR)
      v = 0.f;
    mRmsHeadL = mRmsHeadR = 0;
    mRmsSumL = mRmsSumR = 0.0;
    mVuPosL = mVuVelL = 0.0;
    mVuPosR = mVuVelR = 0.0;
    mDispPeakL = mDispPeakR = -120.f;
    mDispTrueL = mDispTrueR = -120.f;
    mHoldL = mHoldR = -120.f;
    mHoldSec = 0.0;
    mOverL = mOverR = 0;
  }

  // 仅清除峰值保持与过载锁存 (RESET 按钮 / 模式切换)
  void ResetHoldOver() {
    mHoldL = mHoldR = -120.f;
    mOverL = mOverR = 0;
  }

  // 仅清除峰值保持 (模式切换时调用, 过载锁存保留)
  void ResetHold() {
    mHoldL = mHoldR = -120.f;
    mHoldTL = mHoldTR = 0.0;
  }

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
    const int ringSize = (int)mRmsRingL.size();

    float rawPeakL = 0.f, rawPeakR = 0.f;
    float tpPeakL = 0.f, tpPeakR = 0.f;

    for (int i = 0; i < n; ++i) {
      const float l = L[i], r = R[i];
      rawPeakL = std::max(rawPeakL, std::fabs(l));
      rawPeakR = std::max(rawPeakR, std::fabs(r));
      tpPeakL = std::max(tpPeakL, TruePeakSample(l, mHistL));
      tpPeakR = std::max(tpPeakR, TruePeakSample(r, mHistR));
      // RMS 300ms 滑动窗口 (IEC 60268-10 短积分行为, 无泄漏积分收敛误差)
      mRmsSumL += (double)l * l - mRmsRingL[mRmsHeadL];
      mRmsRingL[mRmsHeadL] = l * l;
      mRmsHeadL = (mRmsHeadL + 1) % ringSize;
      mRmsSumR += (double)r * r - mRmsRingR[mRmsHeadR];
      mRmsRingR[mRmsHeadR] = r * r;
      mRmsHeadR = (mRmsHeadR + 1) % ringSize;
      StepVu(mVuPosL, mVuVelL, std::fabs(l), dt, w2dt, wdt2);
      StepVu(mVuPosR, mVuVelR, std::fabs(r), dt, w2dt, wdt2);
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

    // 过载锁存 (用瞬时值判定): dBTP > -1 / dBFS > 0 / VU > +3 VU (-15 dBFS)
    const float overLv = (mode == 0) ? trueDbL : (mode == 1) ? peakDbL : vuDbL;
    const float overRv = (mode == 0) ? trueDbR : (mode == 1) ? peakDbR : vuDbR;
    const float overThr = (mode == 0) ? -1.f : (mode == 1) ? 0.f : -15.f;
    if (overLv > overThr)
      mOverL = 1;
    if (overRv > overThr)
      mOverR = 1;

    // 峰值保持 (基于当前模式主值)
    const float mainL = (mode == 0) ? mDispTrueL : (mode == 1) ? mDispPeakL : vuDbL;
    const float mainR = (mode == 0) ? mDispTrueR : (mode == 1) ? mDispPeakR : vuDbR;
    const float dtBlock = (float)n / (float)sr;
    if (holdSec > 0.0) {
      mHoldL = HoldStep(mHoldL, mainL, dtBlock, rate, holdSec, mHoldTL);
      mHoldR = HoldStep(mHoldR, mainR, dtBlock, rate, holdSec, mHoldTR);
    } else {
      mHoldL = mHoldR = -1000.f; // 无效, UI 不画 hold 线
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
    s.holdDbL = mHoldL;
    s.holdDbR = mHoldR;
    s.holdSec = mHoldSec;
    s.overL = mOverL;
    s.overR = mOverR;
  }

private:
  static constexpr int kPhaseTaps = 12; // 每相位抽头数 (总 4 × 12 = 48 阶)
  static constexpr int kHistLen = kPhaseTaps - 1;

  // 4x 半带插值 FIR 系数 (Blackman 窗 sinc, 各相位直流增益归一化到 1)
  static const std::array<std::array<float, kPhaseTaps>, 4> &PolyCoeffs() {
    static const auto poly = [] {
      constexpr int L = 4 * kPhaseTaps;
      constexpr float kPi = 3.14159265358979323846f;
      const float c = (float)(L - 1) * 0.5f;
      std::array<std::array<float, kPhaseTaps>, 4> p{};
      for (int n = 0; n < L; ++n) {
        const float x = ((float)n - c) / 4.f;
        float h = (std::fabs(x) < 1e-6f) ? 4.f : 4.f * std::sin(kPi * x) / (kPi * x);
        const float w = 0.42f - 0.5f * std::cos(2.f * kPi * (float)n / (float)(L - 1)) +
                        0.08f * std::cos(4.f * kPi * (float)n / (float)(L - 1));
        p[n % 4][n / 4] = h * w;
      }
      for (int k = 0; k < 4; ++k) {
        float s = 0.f;
        for (int j = 0; j < kPhaseTaps; ++j)
          s += p[k][j];
        if (std::fabs(s) > 1e-9f)
          for (int j = 0; j < kPhaseTaps; ++j)
            p[k][j] /= s;
      }
      return p;
    }();
    return poly;
  }

  // 单样本 4x 过采样: 返回该样本 4 个插值输出的最大绝对值, 并推进历史
  static float TruePeakSample(float x, std::array<float, kHistLen> &hist) {
    const auto &poly = PolyCoeffs();
    float ymax = 0.f;
    for (int k = 0; k < 4; ++k) {
      float y = poly[k][0] * x;
      for (int j = 1; j < kPhaseTaps; ++j)
        y += poly[k][j] * hist[j - 1];
      ymax = std::max(ymax, std::fabs(y));
    }
    for (int j = kHistLen - 1; j > 0; --j)
      hist[j] = hist[j - 1];
    hist[0] = x;
    return ymax;
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

  static float AmpToDb(float a) { return (a > 1e-8f) ? 20.f * std::log10(a) : -120.f; }
  static float Db10(double v) { return (v > 1e-12) ? 10.f * (float)std::log10(v) : -120.f; }
  // VU 位置 (平均整流值) -> 正弦 RMS dBFS: RMS = avg * π/(2√2) ≈ 1.1107 * avg
  static float VUToDb(float avg) { return (avg > 1e-8f) ? 20.f * std::log10(1.1107f * avg) : -120.f; }

  double mSR = 48000.0;
  std::array<float, kHistLen> mHistL{};
  std::array<float, kHistLen> mHistR{};
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
  int mOverL = 0, mOverR = 0;
};

} // namespace iplug
