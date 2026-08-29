#pragma once

// HalfbandDec2 — 半带 ×2 抽取器 (PBT / MR-FFT 共用) 及其依赖的倒频谱最小相位谱分解。
// 双系数形态 (SetMinPhase): 线性相位 (默认, 群延迟 = kQ) / 最小相位 (PBT 与 MR-FFT
// 固定使用: 同幅谱分解, 每级群延迟 50→≈7.2 样本) —— 详见 struct 内注。

#ifndef BEGIN_IPLUG_NAMESPACE
#define BEGIN_IPLUG_NAMESPACE namespace iplug {
#define END_IPLUG_NAMESPACE }
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

BEGIN_IPLUG_NAMESPACE
namespace detail {

  // ==== 倒频谱最小相位谱分解 (带核 MINPH 与半波抽取器 MINPH 共用) ====
  inline constexpr double kPi = 3.14159265358979323846;

  // 基-2 迭代 FFT (n 为 2 的幂)。inverse=true 时含 1/n 归一化。
inline void Fft(double *re, double *im, int n, bool inverse) {
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
      const double ang = 2.0 * kPi / (double)len * (inverse ? 1.0 : -1.0);
      const double wr = std::cos(ang), wi = std::sin(ang);
      for (int i = 0; i < n; i += len) {
        double cr = 1.0, ci = 0.0;
        for (int j = 0; j < len / 2; ++j) {
          const double ur = re[i + j], ui = im[i + j];
          const double xr = re[i + j + len / 2], xi = im[i + j + len / 2];
          const double vr = xr * cr - xi * ci;
          const double vi = xr * ci + xi * cr;
          re[i + j] = ur + vr;
          im[i + j] = ui + vi;
          re[i + j + len / 2] = ur - vr;
          im[i + j + len / 2] = ui - vi;
          const double ncr = cr * wr - ci * wi;
          ci = cr * wi + ci * wr;
          cr = ncr;
        }
      }
    }
    if (inverse) {
      for (int i = 0; i < n; ++i) {
        re[i] /= n;
        im[i] /= n;
      }
    }
  }

  // 实对称 FIR 原型 → 同幅频最小相位谱因子 (自相关 + 倒频谱分解), 返回长度 N 不变。
inline constexpr double kMinPhaseDecayTarget = 5e-4;
inline std::vector<double> MinPhaseFactorWithLen(const std::vector<double> &proto, int Lfft, double lift);
inline std::vector<double> MinPhaseFactor(const std::vector<double> &proto,
                                            double stopRipple = std::pow(10.0, -68.0 / 20.0)) {
    const int N = (int)proto.size();
    int Lfft = 1024;
    while (Lfft < 8 * N)
      Lfft <<= 1;
    if (Lfft < 16384)
      Lfft = 16384;
    // 自适应抬底 (见上): Δt = sqrt(ε·Σp²)/δNπ → ε = (Δt·δ·N/π)²/Σp²
    double r0 = 0.0;
    for (double v : proto)
      r0 += v * v;
    const double sEst = stopRipple * N / kPi;
    double lift = (kMinPhaseDecayTarget * sEst) * (kMinPhaseDecayTarget * sEst) / std::max(r0, 1e-30);
    lift = std::clamp(lift, 1e-15, 1e-3);
    std::vector<double> g;
    for (;;) {
      g = MinPhaseFactorWithLen(proto, Lfft, lift);
      double worst = 0.0;
      for (int i = 1; i <= 64; ++i) {
        const double w = kPi * (double)i / 64.0;
        // 旋转递推累加 |A0| 与 |G|: 每频率只算一次 sin/cos, 避开 N·64 次 std::cos。
        // 注意 |A0| 必须取复数模 |Σ proto[n]e^{-jωn}| —— 原型居中对称, 其零相位频响
        // 是该和乘相位因子后的实值; 若只取实部 Σ proto[n]cos(nω) 会多乘 cos(ω·mid),
        // 在其零点附近假性跌百 dB, 校验永远不过 (重试阶梯跑满 → 重建卡 ~9s 的根源)。
        const double cw0 = std::cos(w), sw0 = std::sin(w);
        double ar = 0.0, ai = 0.0, gr = 0.0, gi = 0.0, cw = 1.0, sw = 0.0;
        for (int n = 0; n < N; ++n) {
          ar += proto[n] * cw;
          ai -= proto[n] * sw;
          gr += g[n] * cw;
          gi -= g[n] * sw;
          const double nc = cw * cw0 - sw * sw0;
          sw = cw * sw0 + sw * cw0;
          cw = nc;
        }
        const double dbA = 20.0 * std::log10(std::max(std::sqrt(ar * ar + ai * ai), 1e-15));
        if (dbA < -60.0)
          continue; // 零陷点: 抬底钳制区内, 不参与校验
        const double dbG = 20.0 * std::log10(std::max(std::sqrt(gr * gr + gi * gi), 1e-15));
        worst = std::max(worst, std::abs(dbA - dbG));
      }
      if (worst < 1.0 || Lfft >= (1 << 19))
        break;
      Lfft <<= 1; // 正常一步收敛, 此阶梯仅为正确性兜底
    }
    return g;
  }

  // 指定 FFT 长度与抬底量的倒频谱谱分解 (MinPhaseFactor 的单次尝试)
inline std::vector<double> MinPhaseFactorWithLen(const std::vector<double> &proto, int Lfft, double lift) {
    const int N = (int)proto.size(); // 2M+1
    const int Mr = N - 1;            // 自相关最高滞后 2M (= N−1)
    std::vector<double> re(Lfft, 0.0), im(Lfft, 0.0);
    // 自相关 r[n] = Σ_k proto[k]·proto[k−n], 零相位装载 (bin n 与 bin Lfft−n)
    for (int n = 0; n <= Mr; ++n) {
      double acc = 0.0;
      for (int k = n; k < N; ++k)
        acc += proto[k] * proto[k - n];
      re[n] = acc;
      if (n > 0)
        re[Lfft - n] = acc;
    }
    re[0] *= (1.0 + lift); // 抬底座: 圆上零点 → 圆内 (见 MinPhaseFactor 注)
    Fft(re.data(), im.data(), Lfft, false); // R = A² (实, ≥0)
    // log R (floor 兜底) → IFFT 得实倒频谱
    for (int k = 0; k < Lfft; ++k) {
      re[k] = std::log(std::max(re[k], 1e-24));
      im[k] = 0.0;
    }
    Fft(re.data(), im.data(), Lfft, true);
    // 复倒频谱因果化。注意: 这里的 c = IDFT(log R) = IDFT(log|G|²) 已是标准实倒频谱
    // 的 2 倍 (c[n] = ĉ[n] + ĉ[−n] 且 ĉ 因果 → ĉ[0] = c[0]/2, ĉ[n] = c[n], n ≥ 1),
    // 不可再按 log|G| 的经典公式加倍 —— 加倍等效于对因子再平方, 支撑翻倍, 截断即毁。
    re[0] *= 0.5;
    for (int n = Lfft / 2; n < Lfft; ++n) {
      re[n] = 0.0;
      im[n] = 0.0;
    }
    Fft(re.data(), im.data(), Lfft, false);
    // exp → 最小相位频谱 → IFFT 得因果因子 g (支撑 0..2M, 能量前置)
    for (int k = 0; k < Lfft; ++k) {
      const double er = std::exp(re[k]);
      re[k] = er * std::cos(im[k]);
      im[k] = er * std::sin(im[k]);
    }
    Fft(re.data(), im.data(), Lfft, true);
    std::vector<double> g(Mr + 1);
    for (int n = 0; n <= Mr; ++n)
      g[n] = re[n];
    return g;
  }

// 半带 ×2 抽取器 (跨帧保持滤波状态)。要求 nin 为偶数。系数形态与群延迟见 struct 内注。
struct HalfbandDec2 {
  // 系数两种形态 (SetMinPhase 选择; PBT 固定用最小相位, MR-FFT 共用本结构保持线性相位默认):
  static constexpr int kN = 101;
  static constexpr int kQ = (kN - 1) / 2;
  std::array<float, kN> mTap{};
  std::array<float, kN - 1> mState{};
  float mWork[kN - 1 + 2048];
  bool mMinPhase = false;

  // 对称半带原型 (sinc × BH4 窗; 偶序抽头除中心外严格为零)
  static void BuildProtoTaps(double *taps) {
    for (int i = 0; i < kN; ++i) {
      const int n = i - kQ;
      double v;
      if (n == 0)
        v = 0.5;
      else if ((n & 1) == 0)
        v = 0.0;
      else
        v = 0.5 * std::sin(kPi * n / 2.0) / (kPi * n / 2.0);
      const double theta = 2.0 * kPi * i / (kN - 1); // 4 项 Blackman-Harris: 旁瓣 -92 dB
      v *= 0.35875 - 0.48829 * std::cos(theta)
                   + 0.14128 * std::cos(2.0 * theta)
                   - 0.01168 * std::cos(3.0 * theta);
      taps[i] = v;
    }
  }

  // MINPH 形态的系数 (分解只算一次, 全体抽取器共享; Σg=Σp 由分解保证, 归一化兜底)
  static const std::array<float, kN> &MinPhaseTaps() {
    static const std::array<float, kN> kTaps = [] {
      double taps[kN];
      BuildProtoTaps(taps);
      std::vector<double> proto(taps, taps + kN);
      proto = MinPhaseFactor(proto, std::pow(10.0, -92.0 / 20.0));
      double sum = 0.0;
      for (double v : proto)
        sum += v;
      std::array<float, kN> out{};
      for (int i = 0; i < kN; ++i)
        out[i] = (float)(proto[i] / sum);
      return out;
    }();
    return kTaps;
  }

  // 每级有效群延迟 (本级样本, 最小相位形态): 分解后冲激的能量重心
  // (τ(ω) 随频率 3.9@DC → 15.6@0.45π, 取整体重心做链对齐常数, 端到端验证无对齐回归;
  //  线性相位形态每级 = kQ)
  static double GdPerStage() {
    static const double kCent = [] {
      const std::array<float, kN> &t = MinPhaseTaps();
      double e = 0.0, c = 0.0;
      for (int n = 0; n < kN; ++n) {
        e += (double)t[n] * t[n];
        c += n * (double)t[n] * t[n];
      }
      return c / e;
    }();
    return kCent;
  }

  HalfbandDec2() { RebuildTaps(); }

  // 换系数形态 (PBT 的 RebuildBands 固定切到最小相位; 滤波状态由其尾部 ResetRuntimeState 清零)
  void SetMinPhase(bool mp) {
    if (mp == mMinPhase)
      return;
    mMinPhase = mp;
    RebuildTaps();
  }

  void RebuildTaps() {
    if (mMinPhase) {
      mTap = MinPhaseTaps();
      return;
    }
    double taps[kN];
    BuildProtoTaps(taps);
    double sum = 0.0;
    for (double v : taps)
      sum += v;
    for (int i = 0; i < kN; ++i)
      mTap[i] = (float)(taps[i] / sum);
  }

  void Reset() { mState.fill(0.f); }

  int Process(const float *in, int nin, float *out) {
    const int sz = (kN - 1) + nin;
    for (int i = 0; i < kN - 1; ++i)
      mWork[i] = mState[i];
    for (int i = 0; i < nin; ++i)
      mWork[(kN - 1) + i] = in[i];
    const int nout = nin / 2;
    for (int j = 0; j < nout; ++j) {
      const int p = 2 * j;
      float acc = 0.f;
      for (int i = 0; i < kN; ++i)
        acc += mTap[i] * mWork[(kN - 1) + p - i];
      out[j] = acc;
    }
    for (int i = 0; i < kN - 1; ++i)
      mState[i] = mWork[sz - (kN - 1) + i];
    return nout;
  }
};
} // namespace detail
END_IPLUG_NAMESPACE
