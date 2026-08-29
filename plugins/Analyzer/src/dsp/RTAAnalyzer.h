#pragma once

// RTAAnalyzer — 模拟硬件风格滤波器组实时频谱分析引擎 (Filter-Bank RTA)
//
// 架构对照 seven-phases/spectrum-analyzer: 级联低通差分互补滤波器组。
// N+1 个 2 阶谐振低通, 截止放在带缘 (q = 2·bpo, prewrap 预畸变), 每带 =
// 相邻两个低通输出之差 → 各带构成信号的互补分割 (Σ band = 最高低通),
// 无独立带通滤波器组的响应重叠与重复计数; 检测为逐样本连续功率积分
// (无矩形窗, 低频带无逐 hop 闪动)。带中心增益经解析传输函数校准
// (z=e^{jω0} 稳态模, 与旧仿真校准同值但无瞬态残差, 与其他引擎同一标准)。

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

template <int MAXNC = 3, int QUEUE_SIZE = 64, int MAX_BANDS = 8192>
class RTAAnalyzer : public ISender<MAXNC, QUEUE_SIZE, std::array<float, MAX_BANDS>> {
public:
  using TDataPacket = std::array<float, MAX_BANDS>;
  using Data = ISenderData<MAXNC, TDataPacket>;
  using Base = ISender<MAXNC, QUEUE_SIZE, TDataPacket>;

  static constexpr int kHop = 1024;
  static constexpr int kEnvBlock = 8; // 功率积分块更新长度 (样本; 帧长为其整数倍)
  static constexpr int kMaxBands = 256; // 1/24 Oct 22Hz..20kHz ≈ 236 带 (上限留余量)
  static constexpr double kPi = 3.14159265358979323846;
  static constexpr double kLn2 = 0.69314718055994530942;
  static constexpr double kFreqHi = 20000.0; // 分析上限 (与 VQT/PBT 一致; 高采样率下带表不再上扩)

  enum EOctaveMode {
    kOctave1_6 = 0,  // 1/6 Octave
    kOctave1_12 = 1, // 1/12 Octave
    kOctave1_24 = 2, // 1/24 Octave
    kNumOctaveModes = 3
  };

  // 每倍频程带数与预畸变系数 (按档位; ferr 数值拟合见 RebuildBands 注)
  static constexpr int kBpo[kNumOctaveModes] = {6, 12, 24};
  static constexpr double kFerr[kNumOctaveModes] = {0.68, 0.71, 0.71};

  RTAAnalyzer() {
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

  bool SetOctaveMode(int mode) {
    const int m = std::clamp(mode, 0, (int)kNumOctaveModes - 1);
    if (m != mOctaveMode) {
      mOctaveMode = m;
      mNeedRebuild.store(true, std::memory_order_release);
      return true;
    }
    return false;
  }

  int GetOctaveMode() const { return mOctaveMode; }

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

  // 音频线程: 采集输入采样 (每 kHop 个样本入队一帧原始 hop 数据)
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

  // Freeze (冻结) 支持: UI 线程离线分析一帧原始样本 (与实时路径共用实现)
  void PrepareFrameUI(Data &d) { PrepareDataForUI(d); }

  // Freeze (冻结) 支持: 复位分析侧运行态 (低通状态/功率积分/输入预处理)
  void ResetRuntimeState() {
    for (int c = 0; c < MAXNC; ++c) {
      std::fill(mZ0[c].begin(), mZ0[c].end(), 0.f);
      std::fill(mZ1[c].begin(), mZ1[c].end(), 0.f);
      std::fill(mEnv[c].begin(), mEnv[c].end(), 0.f);
      std::fill(mAcc[c].begin(), mAcc[c].end(), 0.f);
      mPrevIn[c] = 0.f;
    }
  }

  // Freeze (冻结) 支持: 输入侧 hop 相位查询
  int HopPhase() const { return mBufCount; }

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

    const int nLp = nb + 1;
    const float *k0 = mK0.data(), *k1 = mK1.data(), *k2 = mK2.data();
    const float *invGain = mInvGain.data();
    const float alphaBlk = mEnvAlphaBlk; // 块更新系数 (每 kEnvBlock 样本)
    constexpr float kInvBlk = 1.f / kEnvBlock;

    for (int c = 0; c < nCh; ++c) {
      if ((c == 0 && !needL) || (c == 1 && !needR) || (c == 2 && !needSum)) {
        for (int b = 0; b < MAX_BANDS; ++b)
          d.vals[c][b] = 0.f;
        continue;
      }

      alignas(16) float raw[kHop];
      std::memcpy(raw, d.vals[c].data(), kHop * sizeof(float));

      float *z0 = mZ0[c].data();
      float *z1 = mZ1[c].data();
      float *env = mEnv[c].data();
      float *acc = mAcc[c].data(); // 块平方累加器 (逐带)
      float prevIn = mPrevIn[c];

      for (int n = 0; n < kHop; ++n) {
        // 输入预处理 (原版 ZeroLP): x[n] + x[n−1], Nyquist 陷波
        const float xin = raw[n];
        const float x = xin + prevIn;
        prevIn = xin;

        float yPrev = x * k0[0] + z0[0] * k1[0] + z1[0] * k2[0];
        z1[0] = z0[0];
        z0[0] = yPrev;

        for (int j = 1; j < nLp; ++j) {
          const float out = x * k0[j] + z0[j] * k1[j] + z1[j] * k2[j];
          z1[j] = z0[j];
          z0[j] = out;
          // band j−1 = LP_j − LP_{j−1}: 互补差分 (相邻带共享低通输出)
          const float bd = out - yPrev;
          acc[j - 1] += bd * bd;
          yPrev = out;
        }

        if ((n & (kEnvBlock - 1)) == (kEnvBlock - 1)) {
          for (int b = 0; b < nb; ++b) {
            float &e = env[b];
            e += (acc[b] * kInvBlk - e) * alphaBlk;
            acc[b] = 0.f;
          }
        }
      }
      mPrevIn[c] = prevIn;

      // 抗 Denormal
      for (int j = 0; j < nLp; ++j) {
        if (std::abs(z0[j]) < 1e-20f)
          z0[j] = 0.f;
        if (std::abs(z1[j]) < 1e-20f)
          z1[j] = 0.f;
      }
      // 帧末读数: 每带一次 sqrt (原逐样本计算仅末次有效, 全部被覆盖)
      for (int b = 0; b < nb; ++b)
        d.vals[c][b] = std::sqrt(2.f * env[b]) * invGain[b];

      for (int j = 0; j < nb; ++j) {
        if (env[j] < 1e-20f)
          env[j] = 0.f;
        if (acc[j] < 1e-20f)
          acc[j] = 0.f;
      }

      for (int b = nb; b < MAX_BANDS; ++b)
        d.vals[c][b] = 0.f;
    }
  }

private:
  void RebuildBands() {
    mFreqs.clear();
    mK0.clear();
    mK1.clear();
    mK2.clear();
    mInvGain.clear();

    const double fs = std::max(mSampleRate, 1.0);
    const int bpo = kBpo[mOctaveMode];
    // 预畸变系数 ferr (数值拟合): 使相邻带实际交叉频率对齐设计网格带缘,
    // 全采样率 (44.1/48/96/192k) 最坏偏差 ≤0.0005 oct @1/3 (原版 0.904/0.905/0.909 最坏 0.021 oct)
    const double ferr = kFerr[mOctaveMode];

    // 带中心/带缘网格 (原版 update()): 中心 15.625·2^(j/bpo) 自 22Hz 起, 带缘取
    // 几何中点并加 Nyquist 扭曲项; 上缘超 fedg = 0.47·fs 或中心超 20kHz 的带不收录
    const double k = std::exp(kLn2 / bpo);
    double f = 15.625;
    while (f < 22.0)
      f *= k;
    const double fedg = 0.94 * 0.5 * fs;
    const int wi = (int)(0.5 + std::log(fedg / (1000.0 * std::sqrt(k))) / std::log(k));
    const double fli = 1.0 / (1000.0 * std::pow(k, wi));
    const double wr = (1.0 - (fedg / std::sqrt(k)) * fli) * fli * fli;

    double edges[kMaxBands + 1];
    double centers[kMaxBands];
    int nb = 0;
    double ff = (f / std::sqrt(k)) * (1.0 - wr * f * f); // edges[0]: 首中心下缘
    for (;;) {
      const double top = f * std::sqrt(k) * (1.0 - wr * f * f);
      if (nb >= kMaxBands || f > kFreqHi || top > fedg + 1.0)
        break;
      edges[nb] = ff;
      centers[nb] = f;
      ++nb;
      ff = top;
      f *= k;
    }
    edges[nb] = ff; // 顶缘 (最后计入带的上缘; break 路径不写会导致校准读未初始化栈)

    // 边缘低通系数 (原版 twoPoleLPCoeffs): q = 2·bpo, prewrap = ferr
    mK0.resize(nb + 1);
    mK1.resize(nb + 1);
    mK2.resize(nb + 1);
    for (int j = 0; j <= nb; ++j) {
      const double w = 2.0 * kPi * edges[j] / fs;
      const double y = std::sin(w) / ((2.0 * bpo) * (1.0 + std::cos(w * ferr)));
      const double a = 1.0 / (1.0 + y);
      mK2[j] = (float)(a * (y - 1.0));
      mK1[j] = (float)(a * 2.0 * std::cos(w));
      mK0[j] = (float)((0.5 / (2.0 * bpo)) * (1.0 - mK1[j] - mK2[j]));
    }

    // 带中心增益解析校准: 稳态增益 = |(1+z⁻¹)·(LP_{j+1}−LP_j)| 在 z=e^{jω0} 的模。
    // 与旧仿真校准 (中心正弦稳态测差分 rms) 收敛值一致 —— 同一拓扑同一系数,
    // 但无瞬态残差、无 O(fs·Q/f0) 的仿真开销 (1/24 档 192k 下重建仍为瞬时)。
    mInvGain.resize(nb);
    for (int j = 0; j < nb; ++j) {
      const double w0 = 2.0 * kPi * centers[j] / fs;
      const double zr = std::cos(w0), zi = -std::sin(w0);          // z⁻¹ = e^{−jω0}
      const double z2r = std::cos(2.0 * w0), z2i = -std::sin(2.0 * w0); // z⁻²
      const double k0Lo = mK0[j], k1Lo = mK1[j], k2Lo = mK2[j];
      const double k0Hi = mK0[j + 1], k1Hi = mK1[j + 1], k2Hi = mK2[j + 1];
      // A(z) = 1 − k1·z⁻¹ − k2·z⁻²
      const double aLoRe = 1.0 - k1Lo * zr - k2Lo * z2r;
      const double aLoIm = -(k1Lo * zi + k2Lo * z2i);
      const double aHiRe = 1.0 - k1Hi * zr - k2Hi * z2r;
      const double aHiIm = -(k1Hi * zi + k2Hi * z2i);
      // num = k0Hi·A_lo − k0Lo·A_hi, den = A_lo·A_hi
      const double numRe = k0Hi * aLoRe - k0Lo * aHiRe;
      const double numIm = k0Hi * aLoIm - k0Lo * aHiIm;
      const double denRe = aLoRe * aHiRe - aLoIm * aHiIm;
      const double denIm = aLoRe * aHiIm + aLoIm * aHiRe;
      const double denSq = denRe * denRe + denIm * denIm;
      const double qRe = (denSq > 0.0) ? (numRe * denRe + numIm * denIm) / denSq : 0.0;
      const double qIm = (denSq > 0.0) ? (numIm * denRe - numRe * denIm) / denSq : 0.0;
      // H = (1 + z⁻¹)·q, 1 + z⁻¹ = 1 + cos w0 − j·sin w0
      const double oneRe = 1.0 + zr, oneIm = -std::sin(w0);
      const double hRe = oneRe * qRe - oneIm * qIm;
      const double hIm = oneRe * qIm + oneIm * qRe;
      const double g = std::sqrt(hRe * hRe + hIm * hIm);
      mInvGain[j] = (float)(1.0 / std::max(g, 1e-9));
      mFreqs.push_back(centers[j]);
    }

    // 连续功率积分时间常数 (τ = 50ms): 消矩形窗闪动, 瞬态仍由 pad 弹道呈现
    mEnvAlphaBlk = (float)(1.0 - std::exp(-(double)kEnvBlock / (fs * 0.05))); // 块更新系数 (τ = 50ms)

    for (int c = 0; c < MAXNC; ++c) {
      mZ0[c].assign(nb + 1, 0.f);
      mZ1[c].assign(nb + 1, 0.f);
      mEnv[c].assign(nb, 0.f);
      mAcc[c].assign(nb, 0.f);
    }
    mPrevIn.fill(0.f);
    ResetRuntimeState();
  }

  double mSampleRate = 48000.0;
  int mOctaveMode = kOctave1_6; // 0=1/6 (LOW), 1=1/12 (MID), 2=1/24 (HIGH) (与插件参数默认一致)
  int mChanTri = 0;             // 0=LR, 1=PWR, 2=SUM
  std::atomic<bool> mNeedRebuild{false};
  float mEnvAlphaBlk = 0.f; // 功率积分块更新系数 (每 kEnvBlock 样本, τ = 50ms)

  std::vector<double> mFreqs;
  std::vector<float> mK0, mK1, mK2; // 边缘低通系数 (nb+1 组)
  std::vector<float> mInvGain;      // 带中心校准增益 (nb)
  std::array<std::vector<float>, MAXNC> mZ0, mZ1; // 低通状态 (nb+1)
  std::array<std::vector<float>, MAXNC> mEnv;     // 连续功率积分 (nb)
  std::array<std::vector<float>, MAXNC> mAcc;     // 块平方累加器 (nb)
  std::array<float, MAXNC> mPrevIn{};             // 输入预处理一阶状态
  std::array<std::vector<float>, MAXNC> mPending;
  int mBufCount = 0;
};

END_IPLUG_NAMESPACE
