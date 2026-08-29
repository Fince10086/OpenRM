#pragma once

// PAZAnalyzer — 心理声学临界频带频谱分析引擎 (多速率解调核算法)
//
// 每个频带一个复数解调 FIR 核: kernel[n] = proto[n]·e^{-j2π·fc·n/fsL}。
// 核形状: Kaiser 等波纹原型低通, 核长系数 K=4.6
//
// 多速率: 核跑在 2x 半带抽取金字塔上 (层 L 速率 fs/2^L, 跨帧保持滤波状态)
//
// 最小相位化: 带核原型与半波抽取链都做同幅频谱分解 (倒频谱法)

#ifndef STANDALONE_TEST
#include "ISender.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

BEGIN_IPLUG_NAMESPACE

#ifndef O_RM_HALFBAND_DEC2_DEFINED
#define O_RM_HALFBAND_DEC2_DEFINED
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
  // 系数两种形态 (SetMinPhase 选择; PAZ 固定用最小相位, MR-FFT 共用本结构保持线性相位默认):
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

  // 换系数形态 (PAZ 的 RebuildBands 固定切到最小相位; 滤波状态由其尾部 ResetRuntimeState 清零)
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
#endif

template <int MAXNC = 3, int QUEUE_SIZE = 64, int MAX_BANDS = 8192>
class PAZAnalyzer : public ISender<MAXNC, QUEUE_SIZE, std::array<float, MAX_BANDS>> {
public:
  using TDataPacket = std::array<float, MAX_BANDS>;
  using Data = ISenderData<MAXNC, TDataPacket>;
  using Base = ISender<MAXNC, QUEUE_SIZE, TDataPacket>;

  static constexpr int kHop = 1024;
  static constexpr int kMaxLayers = 10;
  static constexpr double kGuard = 0.78; // band 上边距该层新奈奎斯特的比例 (防抽取混叠, VQT 同款)

  PAZAnalyzer() {
    for (int c = 0; c < MAXNC; ++c)
      mPending[c].assign(kHop, 0.f);
    RebuildBands();
  }

  void SetSampleRate(double sr) {
    if (std::abs(mSampleRate - sr) > 0.1) {
      mSampleRate = sr;
      mNeedRebuild.store(true, std::memory_order_release);
    }
  }

  bool SetLfWidth(int widthHz) {
    int mode = 0;
    if (widthHz <= 10)
      mode = 2;
    else if (widthHz <= 20)
      mode = 1;
    else
      mode = 0;

    if (mode != mLfMode) {
      mLfMode = mode;
      mNeedRebuild.store(true, std::memory_order_release);
      return true;
    }
    return false;
  }

  // 设置声道模式 (0: LR, 1: PWR, 2: SUM)
  void SetChannelMode(int chanTri) {
    mChanTri = std::clamp(chanTri, 0, 2);
  }

  void CheckRebuild() {
    if (mNeedRebuild.exchange(false, std::memory_order_acq_rel)) {
      RebuildBands();
    }
  }

  int NumBands() const { return (int)mFreqs.size(); }
  const std::vector<double> &BandFreqs() const { return mFreqs; }

  // 音频线程收集样本
  void ProcessBlock(sample **inputs, int nFrames, int ctrlTag = kNoTag, int nChans = MAXNC,
                    int chanOffset = 0) {
    const int nCh = std::min(nChans, MAXNC);
    for (int s = 0; s < nFrames; ++s) {
      for (int c = 0; c < nCh; ++c)
        mPending[c][mBufCount] = (float)inputs[chanOffset + c][s];
      if (++mBufCount == kHop) {
        Data d{ctrlTag, nCh, chanOffset};
        for (int c = 0; c < nCh; ++c)
          std::copy(mPending[c].begin(), mPending[c].begin() + kHop, d.vals[c].begin());
        Base::PushData(d);
        mBufCount = 0;
      }
    }
  }

// Freeze (冻结) 支持: UI 线程离线分析一帧原始样本 (冻结重算预热, 与实时路径共用实现)
  void PrepareFrameUI(Data &d) { PrepareDataForUI(d); }

  // Freeze (冻结) 支持: 复位分析侧运行态 (层缓冲/抽取器), 不动输入侧
  // mBufCount/mPending 与 band 表 —— 冻结回放的确定性起点 (等价于引擎冷启动)
  void ResetRuntimeState() {
    for (int c = 0; c < MAXNC; ++c) {
      for (int l = 0; l < kMaxLayers; ++l)
        mLayers[c][l].clear();
      for (int l = 0; l < kMaxLayers - 1; ++l)
        mDecim[c][l].Reset();
    }
  }

  // Freeze (冻结) 支持: 输入侧 hop 相位查询 (音频线程 pending 计数, 冻结回放帧格对齐用)
  int HopPhase() const { return mBufCount; }

#ifdef STANDALONE_TEST
  void TestProcessHop(Data &d) {
    PrepareDataForUI(d);
  }
  // 离线验收钩子: 暴露谱分解与贝塞尔函数供数值探针比对
  static std::vector<double> TestMinPhaseFactor(const std::vector<double> &proto) {
    return detail::MinPhaseFactor(proto);
  }
  static double TestBesselI0(double x) { return BesselI0(x); }
#endif

protected:
  // UI 线程计算各频带能量
  void PrepareDataForUI(Data &d) override {
    CheckRebuild();
    const int nb = NumBands();
    if (nb <= 0)
      return;
    const int nCh = std::min(d.nChans, MAXNC);
    const bool needL = (mChanTri != 2);
    const bool needR = (mChanTri != 2);
    const bool needSum = (mChanTri == 2);

    for (int c = 0; c < nCh; ++c) {
      if ((c == 0 && !needL) || (c == 1 && !needR) || (c == 2 && !needSum)) {
        for (int b = 0; b < MAX_BANDS; ++b)
          d.vals[c][b] = 0.f;
        continue;
      }

      // 构建多速率降采样金字塔
      AppendLayer(c, 0, d.vals[c].data(), kHop);
      const float *cur = d.vals[c].data();
      int nin = kHop;
      for (int l = 0; l < kMaxLayers - 1; ++l) {
        float *dst = mScratch[(l & 1)].data();
        nin = mDecim[c][l].Process(cur, nin, dst);
        AppendLayer(c, l + 1, dst, nin);
        cur = dst;
      }

      // band 点积 (复数解调核相关, 幅度 = |Σ x·w·e^{jωn}|·2/Σw)
      for (int b = 0; b < nb; ++b) {
        const Band &bd = mBands[b];
        const std::vector<float> &buf = mLayers[c][bd.layer];
        const int wl = bd.winLen;
        // readOff 夹非负 + 冷启动短缓冲部分窗 (零窗长跳过): 取窗不越界
        const int have = std::max(0, std::min(wl, (int)buf.size() - bd.readOff));
        const int skip = wl - have;
        const int start = std::max(0, (int)buf.size() - bd.readOff - have);
        const float *x = buf.data() + start;
        const float *kr = bd.kernelRe.data() + skip;
        const float *ki = bd.kernelIm.data() + skip;
        const int cnt = wl - skip;
        float re0 = 0.f, re1 = 0.f, re2 = 0.f, re3 = 0.f;
        float im0 = 0.f, im1 = 0.f, im2 = 0.f, im3 = 0.f;
        int j = 0;
        for (; j + 4 <= cnt; j += 4) {
          const float v0 = x[j], v1 = x[j + 1], v2 = x[j + 2], v3 = x[j + 3];
          re0 += v0 * kr[j];     im0 += v0 * ki[j];
          re1 += v1 * kr[j + 1]; im1 += v1 * ki[j + 1];
          re2 += v2 * kr[j + 2]; im2 += v2 * ki[j + 2];
          re3 += v3 * kr[j + 3]; im3 += v3 * ki[j + 3];
        }
        float re = (re0 + re1) + (re2 + re3);
        float im = (im0 + im1) + (im2 + im3);
        for (; j < cnt; ++j) {
          re += x[j] * kr[j];
          im += x[j] * ki[j];
        }
        d.vals[c][b] = std::sqrt(re * re + im * im) * bd.wsumInv;
      }

      for (int b = nb; b < MAX_BANDS; ++b)
        d.vals[c][b] = 0.f;
    }
  }

private:
  struct Band {
    int layer;                            // 速率层 (层 L 速率 = fs/2^L)
    int winLen;                           // 本层速率核长
    int readOff;                          // 跨层延迟对齐: 取窗终点距缓冲末端的本层样本数
    float wsumInv;                        // 2/Σw (带中心正弦幅度归一)
    std::vector<float> kernelRe, kernelIm; // 本层速率的 w·cos/w·sin
  };

  static constexpr double kPi = 3.14159265358979323846;

  static constexpr double kPazFreqs40[52] = {
      23.0, 70.0, 117.0, 164.0, 211.0, 258.0, 305.0, 352.0, 422.0, 516.0,
      609.0, 703.0, 797.0, 891.0, 984.0, 1078.0, 1219.0, 1406.0, 1594.0, 1781.0,
      1969.0, 2156.0, 2344.0, 2531.0, 2719.0, 2906.0, 3188.0, 3563.0, 3937.0, 4312.0,
      4687.0, 5063.0, 5437.0, 5813.0, 6187.0, 6563.0, 6938.0, 7313.0, 7875.0, 8625.0,
      9375.0, 10125.0, 10875.0, 11625.0, 12750.0, 14250.0, 15750.0, 17250.0, 18750.0, 20250.0,
      21750.0, 23250.0
  };

  static constexpr double kPazFreqs20[60] = {
      12.0, 35.0, 59.0, 82.0, 105.0, 129.0, 152.0, 176.0, 211.0, 258.0,
      305.0, 352.0, 398.0, 445.0, 492.0, 539.0, 609.0, 703.0, 797.0, 891.0,
      984.0, 1078.0, 1172.0, 1266.0, 1359.0, 1453.0, 1594.0, 1781.0, 1969.0, 2156.0,
      2344.0, 2531.0, 2719.0, 2906.0, 3094.0, 3281.0, 3469.0, 3656.0, 3937.0, 4312.0,
      4687.0, 5063.0, 5437.0, 5813.0, 6375.0, 7125.0, 7875.0, 8625.0, 9375.0, 10125.0,
      10875.0, 11625.0, 12750.0, 14250.0, 15750.0, 17250.0, 18750.0, 20250.0, 21750.0, 23250.0
  };

  static constexpr double kPazFreqs10[68] = {
      6.0, 18.0, 29.0, 41.0, 53.0, 64.0, 76.0, 88.0, 105.0, 129.0,
      152.0, 176.0, 199.0, 223.0, 246.0, 270.0, 305.0, 352.0, 398.0, 445.0,
      492.0, 539.0, 586.0, 633.0, 680.0, 727.0, 797.0, 891.0, 984.0, 1078.0,
      1172.0, 1266.0, 1359.0, 1453.0, 1547.0, 1641.0, 1734.0, 1828.0, 1969.0, 2156.0,
      2344.0, 2531.0, 2719.0, 2906.0, 3188.0, 3563.0, 3937.0, 4312.0, 4687.0, 5063.0,
      5437.0, 5813.0, 6375.0, 7125.0, 7875.0, 8625.0, 9375.0, 10125.0, 10875.0, 11625.0,
      12750.0, 14250.0, 15750.0, 17250.0, 18750.0, 20250.0, 21750.0, 23250.0
  };

  // 频带依频率升序 → 层号非增, 同层带构成连续段
  int AssignLayer(double bandHi, double fs) const {
    int L = 0;
    while (L + 1 < kMaxLayers) {
      const double newNyq = fs / (double)(1 << (L + 2));
      if (bandHi > kGuard * newNyq)
        break;
      ++L;
    }
    return L;
  }

  // 半带链总群延迟 (输入样本): 每级 GdPerStage() (最小相位能量重心 ≈7.2), 到层 L 共 Σ gd·2^k
  double ChainGd(int L) const {
    return detail::HalfbandDec2::GdPerStage() * (double)((1 << L) - 1);
  }

  void RebuildBands() {
    // 半波抽取链固定最小相位形态 (分解因子全局共享只算一次)
    for (int c = 0; c < MAXNC; ++c)
      for (int l = 0; l < kMaxLayers - 1; ++l)
        mDecim[c][l].SetMinPhase(true);
    mBands.clear();
    mFreqs.clear();
    mMaxWinPerLayer.fill(0);
    const double fs = std::max(mSampleRate, 1.0);
    const double scale = fs / 48000.0;
    const double nyqLimit = 0.485 * fs;

    const double *srcFreqs = nullptr;
    int numSrc = 0;
    if (mLfMode == 2) {
      srcFreqs = kPazFreqs10;
      numSrc = 68;
    } else if (mLfMode == 1) {
      srcFreqs = kPazFreqs20;
      numSrc = 60;
    } else {
      srcFreqs = kPazFreqs40;
      numSrc = 52;
    }

    struct Spec {
      double fc, bw;
      int layer;
    };
    std::vector<Spec> spec;
    for (int i = 0; i < numSrc; ++i) {
      const double fc = srcFreqs[i] * scale;
      if (fc >= nyqLimit)
        continue;

      // 带宽取频带表相邻间距 (首末带单边), 与显示网格一致
      double bw;
      if (i == 0)
        bw = (srcFreqs[1] - srcFreqs[0]) * scale;
      else if (i == numSrc - 1)
        bw = (srcFreqs[numSrc - 1] - srcFreqs[numSrc - 2]) * scale;
      else
        bw = 0.5 * (srcFreqs[i + 1] - srcFreqs[i - 1]) * scale;

      Spec sp{fc, bw, 0};
      sp.layer = AssignLayer(fc + bw / 2.0, fs);
      spec.push_back(sp);
    }

    std::array<int, kMaxLayers> layerCount{};
    for (const Spec &sp : spec)
      ++layerCount[sp.layer];

    for (int i = 0, n = (int)spec.size(); i < n;) {
      const int L = spec[i].layer;
      int j = i;
      while (j < n && spec[j].layer == L)
        ++j; // 本层 band 段 [i, j)
      // 下一更深带流层 (段顶 band 对齐到该层基准时刻)
      int nextDeep = -1;
      for (int d = L + 1; d < kMaxLayers; ++d)
        if (layerCount[d] > 0) {
          nextDeep = d;
          break;
        }
      for (int t = i; t < j; ++t) {
        const double fSeg = (j - i > 1) ? (double)(t - i) / (j - i - 1) : 0.0;
        AddBand(spec[t].fc, spec[t].bw, fs, L, nextDeep, fSeg);
      }
      i = j;
    }

    ResetRuntimeState();
  }

  void AddBand(double fc, double bw, double fs, int L, int nextDeep, double fSeg) {
    const double fsL = fs / (double)(1 << L);
    // Kaiser 等波纹原型低通 —— 平顶到 0.37bw, 阻带墙 Esb=0.13·K·bw 起 −68dB
    // (K=4.6 → Esb≈0.60bw, 原版拟合定值); 过渡带越窄核越长: T = (A−8)/(2.285·2π·(Esb−Epb))。
    // 最小相位谱分解要求奇长实对称原型, 偶长 +1。
    int wl;
    const double epb = 0.37 * bw;
    const double esb = std::max(0.13 * kKernelLen * bw, epb + 0.10 * bw); // 过渡带不退化
    const double dF = esb - epb;
    constexpr double kAttenDb = 68.0; // 阻带深度 (= 原版实测远端底 ≈ −70dB)
    wl = std::max(9, (int)std::ceil((kAttenDb - 8.0) / (2.285 * 2.0 * kPi * dF / fsL)));
    if ((wl & 1) == 0)
      ++wl; // 奇长 → 实对称

    // 1) 实对称原型 (中心 mid): Kaiser 窗化 sinc 低通 (DC 增益 1)
    const int mid = (wl - 1) / 2;
    std::vector<double> proto(wl);
    {
      const double beta = 0.1102 * (68.0 - 8.7);
      const double fcLp = 0.5 * (epb + esb);
      const double wC = 2.0 * kPi * fcLp / fsL; // 原型截止 (rad/样本)
      for (int n = 0; n < wl; ++n) {
        const double d = n - mid;
        const double h = (d == 0) ? (wC / kPi) : std::sin(wC * d) / (kPi * d);
        const double t = 2.0 * n / (wl - 1) - 1.0; // −1..1
        const double arg = beta * std::sqrt(std::max(0.0, 1.0 - t * t));
        const double wk = BesselI0(arg) / BesselI0(beta);
        proto[n] = h * wk;
      }
    }

    // 2) 最小相位谱分解 (固有): |G(ω)|=|A(ω)| 全频带逐点成立 (静态响应/零陷不变),
    //    能量前置 —— 起振群延迟塌缩; 核长不变 (自相关法, 见 detail::MinPhaseFactor 注)
    proto = detail::MinPhaseFactor(proto);
    // 时间反转: 点积按 "kernel[0] 配最旧样本" 取向, 卷积等效于时间翻转核 —— 不反转
    // 的话最小相位核等效成最大相位 (能量落在旧样本侧, 起振反而最慢)。反转后能量
    // 前置的抽头配最新样本, 新信号一到立即响应; 幅度响应不受翻转影响 (静态等价)。
    std::reverse(proto.begin(), proto.end());

    Band bd;
    bd.layer = L;
    bd.winLen = (int)proto.size();
    // 跨层延迟对齐: 段顶 band (fSeg=0) 对齐到下一深层基准时刻, 段底 (fSeg=1) 对齐到本层
    const double gdL = ChainGd(L);
    const double gdD = (nextDeep > 0) ? ChainGd(nextDeep) : gdL;
    const double R = gdD + (gdL - gdD) * fSeg;
    // 夹非负: 负 readOff 会让取窗起点越过缓冲区末端 (读越界)
    bd.readOff = std::max(0, (int)std::lround((R - gdL) / (double)(1 << L)));
    // 3) 载波调制 + 归一 (谱分解保持 DC 增益: Σg = A(0) = Σh, 按分解结果实测求和)
    bd.kernelRe.resize(bd.winLen);
    bd.kernelIm.resize(bd.winLen);
    const double step = 2.0 * kPi * fc / fsL; // 该层速率的归一化频率
    double ksum = 0.0;
    for (int n = 0; n < bd.winLen; ++n) {
      const double ph = step * n;
      bd.kernelRe[n] = (float)(proto[n] * std::cos(ph));
      bd.kernelIm[n] = (float)(proto[n] * std::sin(ph));
      ksum += proto[n];
    }
    bd.wsumInv = (ksum > 1e-12) ? (float)(2.0 / ksum) : 0.0f;

    mBands.push_back(std::move(bd));
    mFreqs.push_back(fc);
    mMaxWinPerLayer[L] = std::max(mMaxWinPerLayer[L], bd.winLen + bd.readOff);
  }

  // 层缓冲追加 (保留窗口覆盖所需尾部)
  void AppendLayer(int c, int l, const float *src, int n) {
    std::vector<float> &buf = mLayers[c][l];
    const int maxKeep = mMaxWinPerLayer[l];
    if (n >= maxKeep) {
      buf.assign(src + (n - maxKeep), src + n);
      return;
    }
    const int oldKeep = maxKeep - n;
    if ((int)buf.size() > oldKeep)
      buf.erase(buf.begin(), buf.begin() + (buf.size() - oldKeep));
    buf.insert(buf.end(), src, src + n);
  }

  // 第一类修正贝塞尔 I0 (Kaiser 窗用; 级数展开, x ≤ ~10 收敛快)
  static double BesselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 32; ++k) {
      term *= (x / 2.0) / k * (x / 2.0) / k;
      sum += term;
      if (term < 1e-12 * sum)
        break;
    }
    return sum;
  }

  static constexpr double kKernelLen = 4.6; // 核长系数 k (Kaiser 墙位 Esb=0.13·k·bw), 原版拟合定值
  double mSampleRate = 48000.0;
  int mLfMode = 0; // 0=40Hz, 1=20Hz, 2=10Hz
  int mChanTri = 0; // 0=LR, 1=PWR, 2=SUM
  std::atomic<bool> mNeedRebuild{false};

  std::vector<Band> mBands;
  std::vector<double> mFreqs;
  std::array<int, kMaxLayers> mMaxWinPerLayer{}; // 该层需保留的本层样本数 (核长 + readOff)

  std::array<std::array<detail::HalfbandDec2, kMaxLayers - 1>, MAXNC> mDecim;
  std::array<std::array<std::vector<float>, kMaxLayers>, MAXNC> mLayers;
  std::array<std::array<float, kHop / 2>, 2> mScratch; // 金字塔级间缓冲 (乒乓)

  std::array<std::vector<float>, MAXNC> mPending;
  int mBufCount = 0;
};

END_IPLUG_NAMESPACE
