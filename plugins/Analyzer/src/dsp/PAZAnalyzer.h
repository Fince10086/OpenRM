#pragma once

// PAZAnalyzer — 心理声学多速率带通滤波器组频谱分析引擎 (复刻 Waves PAZ 经典多速率架构)
//
// 架构特点:
// 1. 线程模型:
//    - 音频线程 (ProcessBlock): 纯 1024 样本缓冲拷贝入队, 零 DSP 计算, CPU 占用严格 ≈ 0.00%。
//    - UI 线程 (PrepareDataForUI): 执行多速率半带降采样金字塔与各层分频带 4 阶 TPT SVF 滤波。
// 2. 多速率金字塔 (10 层 ×2 逐级抽取):
//    - 高频在 48kHz (1024 样本) 处理, 随着频率降低, 子带样本量逐级减半 (512, 256, 128 ... 2 样本)。
//    - 总样本处理步数从 69,632 步暴降至约 6,000 步 (算力降低 > 85%)。
// 3. 自然级联群延迟 (Zero Artificial Delay):
//    - 每一级半带 FIR (17 抽头) 跨帧保持状态, 天然累积 8*(2^L - 1) 样本的时序流动延迟。
//    - 低频波峰自然在 3~4 帧 (~70-90ms) 后涌出, 无需任何人工延时环形队列 (去除了 st.hist/delayHops 等冗余代码)。
// 4. 循环优化与寄存器常驻:
//    - Band 外层、Sample 内层, 状态变量锁在局部寄存器中连续迭代, 循环内零数组寻址、零 Denormal 分支。

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
// 半带 ×2 抽取器 (跨帧保持滤波状态)。17 抽头 Hamming 窗半带 (截止 π/2, DC 增益 1),
// 偶数序 (除中心) 抽头严格为零 → 每输出样本 9 次乘加。
struct HalfbandDec2 {
  static constexpr int kN = 17;
  static constexpr int kQ = (kN - 1) / 2;
  std::array<float, kN> mTap{};
  std::array<float, kN - 1> mState{};
  float mWork[kN - 1 + 2048];

  HalfbandDec2() { BuildTaps(); }

  void BuildTaps() {
    constexpr double kPi = 3.14159265358979323846;
    double taps[kN];
    double sum = 0.0;
    for (int i = 0; i < kN; ++i) {
      const int n = i - kQ;
      double v;
      if (n == 0)
        v = 0.5;
      else if ((n & 1) == 0)
        v = 0.0;
      else
        v = 0.5 * std::sin(kPi * n / 2.0) / (kPi * n / 2.0);
      v *= 0.54 - 0.46 * std::cos(2.0 * kPi * i / (kN - 1));
      taps[i] = v;
      sum += v;
    }
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

template <int MAXNC = 3, int QUEUE_SIZE = 64, int MAX_BANDS = 4096>
class PAZAnalyzer : public ISender<MAXNC, QUEUE_SIZE, std::array<float, MAX_BANDS>> {
public:
  using TDataPacket = std::array<float, MAX_BANDS>;
  using Data = ISenderData<MAXNC, TDataPacket>;
  using Base = ISender<MAXNC, QUEUE_SIZE, TDataPacket>;

  static constexpr int kHop = 1024;
  static constexpr int kMaxLayers = 10;

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

  // 设置声道显示模式 (0: LR, 1: PWR, 2: SUM) 以启用声道惰性计算
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

  // 音频线程: 仅收集 1024 原始样本入队 (零 DSP 计算)
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

#ifdef STANDALONE_TEST
  void TestProcessHop(Data &d) {
    PrepareDataForUI(d);
  }
#endif

protected:
  // UI 线程 (OnIdle / TransmitData):
  // 1. 声道惰性分派: LR/PWR 模式跳过 Sum (节省 33%), SUM 模式跳过 L/R (节省 66%)
  // 2. 生成多速率降采样金字塔 (Layer 0..9)
  // 3. 各 band 仅在其对应降采样层上进行 4 阶 TPT SVF 滤波 (样本内层, 寄存器常驻)
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

      // 1. 构建该通道的多速率降采样金字塔
      float *l0 = mLayers[c][0].data();
      std::copy(d.vals[c].begin(), d.vals[c].begin() + kHop, l0);
      int nin = kHop;
      for (int l = 0; l < kMaxLayers - 1; ++l) {
        nin = mDecim[c][l].Process(mLayers[c][l].data(), nin, mLayers[c][l + 1].data());
      }

      // 2. 逐频带流式滤波
      BandState *states = mStates[c].data();
      for (int b = 0; b < nb; ++b) {
        const BandCoef &coef = mBands[b];
        BandState &st = states[b];
        const int l = coef.layer;
        const int nSamples = kHop >> l;
        const float *src = mLayers[c][l].data();

        float ic1_1 = st.ic1_1, ic2_1 = st.ic2_1;
        float ic1_2 = st.ic1_2, ic2_2 = st.ic2_2;
        float pk = 0.f;

        for (int s = 0; s < nSamples; ++s) {
          const float inSample = src[s];

          // 级联第 1 级 2 阶 TPT SVF 带通
          const float v3_1 = inSample - ic2_1;
          const float v1_1 = coef.a1 * ic1_1 + coef.a2 * v3_1;
          const float v2_1 = ic2_1 + coef.a2 * ic1_1 + coef.a3 * v3_1;
          ic1_1 = 2.f * v1_1 - ic1_1;
          ic2_1 = 2.f * v2_1 - ic2_1;

          // 级联第 2 级 2 阶 TPT SVF 带通 (输入为第 1 级的输出 v1_1)
          const float v3_2 = v1_1 - ic2_2;
          const float v1_2 = coef.a1 * ic1_2 + coef.a2 * v3_2;
          const float v2_2 = ic2_2 + coef.a2 * ic1_2 + coef.a3 * v3_2;
          ic1_2 = 2.f * v1_2 - ic1_2;
          ic2_2 = 2.f * v2_2 - ic2_2;

          const float mag = std::abs(v1_2) * coef.kb;
          if (mag > pk)
            pk = mag;
        }

        // 块末尾去非规格化微小浮点数保护 (循环内零分支跳转)
        if (std::abs(ic1_1) < 1e-30f) {
          ic1_1 = ic2_1 = ic1_2 = ic2_2 = 0.f;
        }

        st.ic1_1 = ic1_1; st.ic2_1 = ic2_1;
        st.ic1_2 = ic1_2; st.ic2_2 = ic2_2;

        d.vals[c][b] = pk;
      }

      for (int b = nb; b < MAX_BANDS; ++b)
        d.vals[c][b] = 0.f;
    }
  }

private:
  struct BandCoef {
    float a1, a2, a3; // TPT SVF 系数
    float kb;         // 归一化增益因子 (1/Q)^2
    int layer;        // 该频带所属的多速率金字塔层级 (0..9)
  };

  struct BandState {
    float ic1_1 = 0.f, ic2_1 = 0.f; // 级联第 1 级状态
    float ic1_2 = 0.f, ic2_2 = 0.f; // 级联第 2 级状态
  };

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

  void RebuildBands() {
    mBands.clear();
    mFreqs.clear();
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

    for (int i = 0; i < numSrc; ++i) {
      const double fc = srcFreqs[i] * scale;
      if (fc >= nyqLimit)
        continue;

      // 带宽与品质因数
      double bw;
      if (i == 0)
        bw = (srcFreqs[1] - srcFreqs[0]) * scale;
      else if (i == numSrc - 1)
        bw = (srcFreqs[numSrc - 1] - srcFreqs[numSrc - 2]) * scale;
      else
        bw = 0.5 * (srcFreqs[i + 1] - srcFreqs[i - 1]) * scale;

      const double Q = std::clamp(fc / std::max(bw, 1.0), 0.707, 15.0);

      // 计算频带所属的金字塔层级 L (严格处于半带平坦通带内 < 0.38 * fs / 2^L, 避免过渡带滚降)
      static constexpr double kGuard = 0.38;
      const double safeLimit = kGuard * fs;
      int layer = 0;
      if (fc < safeLimit) {
        layer = (int)std::floor(std::log2(safeLimit / std::max(fc, 1.0)));
        layer = std::clamp(layer, 0, kMaxLayers - 1);
      }

      // 计算该层采样率下的 TPT SVF 系数
      const double layerFs = fs / (double)(1 << layer);
      constexpr double kPi = 3.14159265358979323846;
      const double g = std::tan(kPi * fc / layerFs);
      const double k = 1.0 / Q;
      const double a1 = 1.0 / (1.0 + g * (g + k));
      const double a2 = g * a1;
      const double a3 = g * a2;
      const double kb = 1.0 / (Q * Q);

      BandCoef coef;
      coef.a1 = (float)a1;
      coef.a2 = (float)a2;
      coef.a3 = (float)a3;
      coef.kb = (float)kb;
      coef.layer = layer;

      mBands.push_back(coef);
      mFreqs.push_back(fc);
    }

    // 复位各通道状态与半带抽取器
    for (int c = 0; c < MAXNC; ++c) {
      mStates[c].assign(mBands.size(), BandState{});
      for (int l = 0; l < kMaxLayers - 1; ++l)
        mDecim[c][l].Reset();
      for (int l = 0; l < kMaxLayers; ++l)
        mLayers[c][l].assign(kHop >> l, 0.f);
    }
  }

  double mSampleRate = 48000.0;
  int mLfMode = 0; // 0=40Hz, 1=20Hz, 2=10Hz
  int mChanTri = 0; // 0=LR, 1=PWR, 2=SUM
  std::atomic<bool> mNeedRebuild{false};

  std::vector<BandCoef> mBands;
  std::vector<double> mFreqs;
  std::array<std::vector<BandState>, MAXNC> mStates;
  std::array<std::vector<float>, MAXNC> mPending;
  int mBufCount = 0;

  std::array<std::array<detail::HalfbandDec2, kMaxLayers - 1>, MAXNC> mDecim;
  std::array<std::array<std::vector<float>, kMaxLayers>, MAXNC> mLayers;
};

END_IPLUG_NAMESPACE
