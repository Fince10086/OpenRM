#pragma once

// TestSignalGenerator — 内置测试信号发生器 (开发者工具)
//
// 用途: 在插件内部直接生成已知信号替换分析输入, 用于对比不同频谱算法的优劣。
// 相比外部音源 (VST 乐器 / 音频文件) 的三个决定性优势:
//   1) 样本级确定性: 固定 seed 的 PRNG, 逐样本可复现。
//   2) 时间对齐精确: 与引擎共用同一 nFrames, 脉冲落在第几个样本完全可控 ——
//      这是测量瞬态响应 (上升沿帧数) 的前提, 宿主路由下的外部音源做不到。
//   3) 交叠点可控: 正弦可精确吸附到 FFT bin 中心或两 bin 正中,
//      从而把 scalloping loss (Hann 窗理论 -1.42 dB) 变成可测量的量。
//
// 纯 header, 零依赖 (仅标准库), 便于 tests/ 下的离线基准直接复用同一份信号定义。
// 编译开关: ORM_ENABLE_TEST_GEN (发布包设为 0 即可完全裁掉)。
//
// 注: 状态只能在音频线程上推进 (Fill)。UI 侧改配置走 SetConfig, 不做跨线程共享。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace orm {

enum ETestSignal {
  kGenOff = 0,
  kGenSineBinCenter,  // 正弦吸附到最近 bin 中心 (泄漏最小 → 用于幅度校准)
  kGenSineBinBetween, // 正弦吸附到最近两 bin 正中 (泄漏最坏 → 用于 scalloping loss)
  kGenSine,           // 正弦, 自由频率
  kGenSweepLog,       // 对数扫频 (频响平坦度 / 金字塔交叠台阶)
  kGenWhiteNoise,     // 白噪 (统计平坦度)
  kGenPinkNoise,      // 粉噪 -3 dB/oct (斜率正确性)
  kGenImpulseTrain,   // 周期脉冲 (时间分辨率 / 上升沿)
  kGenDirac,          // 单次冲激 (窗泄漏形状 / 时间对齐)
  kGenTwoTone,        // 双音 (相邻频带分辨与掩蔽)
  kGenSquare,         // 带限方波 (奇次谐波分辨)
  kGenSilence,        // 静音 (底噪 / 数值下限)
  kGenDC,             // 直流 (DC 泄漏)
  kNumGenSignals
};

inline const char *TestSignalName(int type) {
  static const char *const kNames[kNumGenSignals] = {
      "OFF",     "SINE BIN CTR", "SINE BIN MID", "SINE",    "LOG SWEEP", "WHITE", "PINK",
      "IMPULSE", "DIRAC",        "TWO TONE",     "SQUARE",  "SILENCE",   "DC"};
  return kNames[(unsigned)type < (unsigned)kNumGenSignals ? type : 0];
}

class TestSignalGenerator {
public:
  struct Config {
    int type = kGenOff;
    double freqHz = 1000.0; // 正弦/方波/双音基频, 或脉冲串速率 (Hz)
    double levelDb = -12.0; // 输出电平 (dBFS 峰值)
    int fftSize = 4096;     // bin 吸附用的 FFT 网格尺寸 (bin = sr / fftSize)
    double sweepSec = 4.0;  // 对数扫频周期 (s)
    double sweepF0 = 20.0;
    double sweepF1 = 20000.0;
  };

  bool Active() const { return mCfg.type != kGenOff; }
  int Type() const { return mCfg.type; }
  const Config &GetConfig() const { return mCfg; }

  // 音频线程每 block 调用。仅在配置真的变化时做重活;
  // 信号类型切换时自动 Restart (从头开始, 保证可复现)。
  void SetConfig(const Config &c) {
    if (c.type == mCfg.type && c.freqHz == mCfg.freqHz && c.levelDb == mCfg.levelDb &&
        c.fftSize == mCfg.fftSize && c.sweepSec == mCfg.sweepSec && c.sweepF0 == mCfg.sweepF0 &&
        c.sweepF1 == mCfg.sweepF1)
      return;
    const bool typeChanged = (c.type != mCfg.type);
    mCfg = c;
    if (typeChanged)
      Restart();
  }

  void SetSeed(uint32_t seed) {
    mSeed = seed;
    Reseed();
  }
  uint32_t Seed() const { return mSeed; }

  // 信号归零: 样本索引归 0, 相位/滤波器/PRNG 全部复位。
  // 与 Freeze 配合使用: 先 RESTART 再录满环形缓冲再冻结,
  // 得到的就是"从 t=0 起的确定性信号段", 换引擎重算可严格对比。
  void Restart() {
    mIndex = 0;
    mDiracFired = false;
    mImpulseIdx = (uint64_t)-1; // 使 t=0 处立即触发第一次脉冲
    mPinkL.fill(0.0);
    mPinkR.fill(0.0);
    Reseed();
  }

  // 当前信号在给定采样率下的实际频率 (用于 UI 显示吸附后的结果)
  double EffectiveFreqHz(double sr) const {
    if (mCfg.type == kGenSineBinCenter || mCfg.type == kGenSineBinBetween)
      return SnapFreq(mCfg.type, mCfg.freqHz, sr, mCfg.fftSize);
    return mCfg.freqHz;
  }

  template <typename T> void Fill(T *dstL, T *dstR, int n, double sr, bool advance = true) {
    if (!Active() || n <= 0 || sr <= 0.0)
      return;

    const double amp = std::pow(10.0, mCfg.levelDb / 20.0);
    const double invSr = 1.0 / sr;
    const double nyq = 0.5 * sr;

    // 频率吸附: bin 中心 = k·sr/N, bin 之间 = (k+0.5)·sr/N
    const double f = std::clamp(SnapFreq(mCfg.type, mCfg.freqHz, sr, mCfg.fftSize), 0.0, nyq * 0.95);
    const double w = kTwoPi * f;

    for (int i = 0; i < n; ++i) {
      const double t = (double)mIndex * invSr; // 绝对时间, 由样本索引推出 → 跨 block 连续
      double l = 0.0, r = 0.0;

      switch (mCfg.type) {
        case kGenSineBinCenter:
        case kGenSineBinBetween:
        case kGenSine: {
          l = r = amp * std::sin(w * t);
          break;
        }
        case kGenSweepLog: {
          // 相位积分: φ(t) = 2π·f0·T/ln(ratio)·(ratio^(t/T) − 1)
          // 直接写 sin(2π·f(t)·t) 是错的 —— 那样瞬时频率不等于 f(t)。
          const double period = std::max(mCfg.sweepSec, 0.05);
          const double tt = std::fmod(t, period);
          const double ratio = std::max(mCfg.sweepF1, 1e-6) / std::max(mCfg.sweepF0, 1e-6);
          const double kk = std::log(ratio);
          const double ph = kTwoPi * mCfg.sweepF0 * period / kk * (std::exp(tt * kk / period) - 1.0);
          l = r = amp * std::sin(ph);
          break;
        }
        case kGenWhiteNoise: {
          l = amp * White(mRngL);
          r = amp * White(mRngR); // 独立流: L/R 去相关, 便于检验 PWR/SUM 合并
          break;
        }
        case kGenPinkNoise: {
          l = amp * Pink(mPinkL, White(mRngL));
          r = amp * Pink(mPinkR, White(mRngR));
          break;
        }
        case kGenImpulseTrain: {
          const double rate = std::clamp(mCfg.freqHz, 0.5, 50.0);
          const uint64_t idx = (uint64_t)(t * rate);
          const bool fire = (idx != mImpulseIdx);
          if (fire)
            mImpulseIdx = idx;
          l = r = fire ? amp : 0.0;
          break;
        }
        case kGenDirac: {
          const bool fire = !mDiracFired;
          if (fire)
            mDiracFired = true;
          l = r = fire ? amp : 0.0;
          break;
        }
        case kGenTwoTone: {
          // 两个等幅音, 相距 1/3 八度; 各占 amp/2 保证合成峰值不超过 amp
          const double f2 = std::min(f * 1.2599210498948732, nyq * 0.95); // 2^(1/3)
          const double a = amp * 0.5;
          l = r = a * std::sin(w * t) + a * std::sin(kTwoPi * f2 * t);
          break;
        }
        case kGenSquare: {
          l = r = amp * BandLimitedSquare(w * t, f, nyq);
          break;
        }
        case kGenSilence:
          l = r = 0.0;
          break;
        case kGenDC:
          l = r = amp;
          break;
        default:
          l = r = 0.0;
          break;
      }

      dstL[i] = (T)l;
      dstR[i] = (T)r;
      if (advance)
        ++mIndex;
    }
  }

private:
  static constexpr double kTwoPi = 6.283185307179586476925286766559;
  static constexpr double kFourOverPi = 1.2732395447351626861510701069801;

  static double SnapFreq(int type, double freqHz, double sr, int fftSize) {
    if (type != kGenSineBinCenter && type != kGenSineBinBetween)
      return freqHz;
    const double binHz = sr / (double)std::max(fftSize, 16);
    const double k = std::floor(freqHz / binHz + 0.5);
    return (type == kGenSineBinCenter ? k : k + 0.5) * binHz;
  }

  static inline uint32_t Splitmix32(uint32_t x) {
    x += 0x9E3779B9u;
    x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
    x = (x ^ (x >> 13)) * 0xC2B2AE35u;
    return x ^ (x >> 16);
  }

  void Reseed() {
    mRngL = Splitmix32(mSeed);
    mRngR = Splitmix32(mSeed ^ 0x9E3779B9u);
    if (mRngL == 0u)
      mRngL = 0x1234567u; // xorshift 的零状态是吸收态
    if (mRngR == 0u)
      mRngR = 0x89ABCDEu;
  }

  // xorshift32: 状态 4 字节, 音频线程友好; 均匀映射到 [-1, 1)
  static inline double White(uint32_t &s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (double)(int32_t)s * (1.0 / 2147483648.0);
  }

  // 粉噪: Paul Kellet 的 7 极点 -3 dB/oct 近似滤波器。
  // 实测 (48 kHz, 100 Hz-21.6 kHz, 6 seed 平均): 每八度 -3.0 dB, 纹波约 +-0.2 dB。
  static inline double Pink(std::array<double, 7> &b, double w) {
    b[0] = 0.99886 * b[0] + w * 0.0555179;
    b[1] = 0.99332 * b[1] + w * 0.0750759;
    b[2] = 0.96900 * b[2] + w * 0.1538520;
    b[3] = 0.86650 * b[3] + w * 0.3104856;
    b[4] = 0.55000 * b[4] + w * 0.5329522;
    b[5] = -0.7616 * b[5] - w * 0.0168980;
    const double out = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362;
    b[6] = w * 0.115926;
    return out * 0.11;
  }

  // 带限方波: (4/π)·Σ_{k odd} sin(k·x)/k, 谐波截断在 0.9·Nyquist 以下。
  // 不用朴素 sign(sin x) —— 那会把混叠折叠回分析频段, 污染底噪与谐波的判读。
  // 谐波正弦走二项递推 sin((k+2)x) = 2cos(2x)sin(kx) − sin((k−2)x), 每样本只一次 sin/cos。
  static double BandLimitedSquare(double x, double f, double nyq) {
    if (f <= 0.0)
      return 0.0;
    const int maxK = std::min((int)std::floor(0.9 * nyq / f), 63);
    double sKm2 = -std::sin(x); // sin((k−2)x), k=1 时为 sin(−x)
    double sK = std::sin(x);
    double sum = sK;
    if (maxK < 3)
      return kFourOverPi * sum;
    const double c2 = std::cos(2.0 * x);
    for (int k = 3; k <= maxK; k += 2) {
      const double sNext = 2.0 * c2 * sK - sKm2;
      sKm2 = sK;
      sK = sNext;
      sum += sNext / (double)k;
    }
    return kFourOverPi * sum;
  }

  Config mCfg{};
  uint32_t mSeed = 0x12345678u;
  uint32_t mRngL = 0x1234567u;
  uint32_t mRngR = 0x89ABCDEu;
  std::array<double, 7> mPinkL{};
  std::array<double, 7> mPinkR{};
  uint64_t mIndex = 0;
  uint64_t mImpulseIdx = (uint64_t)-1;
  bool mDiracFired = false;
};

} // namespace orm
