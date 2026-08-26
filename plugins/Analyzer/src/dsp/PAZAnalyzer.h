#pragma once

// PAZAnalyzer — 心理声学带通滤波器组频谱分析引擎 (复刻 Waves PAZ 经典架构)
//
// 特性:
// 1. 频带划分: 严格复刻 PAZ 规范 (< 250 Hz 为 40/20/10 Hz 等带宽线性划分, ≥ 250 Hz 为恒定 Q=10 等比递推)
//    - 40 Hz 档: 52 bands (原厂说明书精确数值)
//    - 20 Hz 档: 58 bands
//    - 10 Hz 档: 69 bands
// 2. 滤波器拓扑: 每频带采用双级联 4 阶 TPT SVF (双二阶拓扑保持状态变量带通),
//    相邻频带隔离度高达 13.2 ~ 26 dB, 完美呈现 PAZ 标志性的陡峭嶙峋峰谷与锯齿感。
// 3. 时间全密度: 音频线程连续逐样本滤波, 绝无时间空洞, 脉冲 100% 稳定捕获。
// 4. 幅度提取: 1024 样本 (21.3 ms) Hop 内整流峰值提取, 杜绝低频交流纹波呼吸抖动。

#ifndef STANDALONE_TEST
#include "ISender.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

BEGIN_IPLUG_NAMESPACE

template <int MAXNC = 3, int QUEUE_SIZE = 64, int MAX_BANDS = 128>
class PAZAnalyzer : public ISender<MAXNC, QUEUE_SIZE, std::array<float, 4096>> {
public:
  using TDataPacket = std::array<float, 4096>;
  using Data = ISenderData<MAXNC, TDataPacket>;
  using Base = ISender<MAXNC, QUEUE_SIZE, TDataPacket>;

  static constexpr int kHop = 1024;
  static constexpr double kFreqLo = 20.0;
  static constexpr double kFreqHi = 20000.0;

  PAZAnalyzer() {
    RebuildBands();
  }

  void SetSampleRate(double sr) {
    if (std::abs(mSampleRate - sr) > 0.1) {
      mSampleRate = sr;
      mNeedRebuild.store(true, std::memory_order_release);
    }
  }

  // 设置低频分辨率档位: 0=40Hz (52 bands), 1=20Hz (58 bands), 2=10Hz (69 bands)
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

  void CheckRebuild() {
    if (mNeedRebuild.exchange(false, std::memory_order_acq_rel)) {
      std::lock_guard<std::mutex> lock(mMutex);
      RebuildBands();
    }
  }

  int NumBands() {
    std::lock_guard<std::mutex> lock(mMutex);
    return (int)mFreqs.size();
  }

  std::vector<float> BandFreqs() {
    std::lock_guard<std::mutex> lock(mMutex);
    std::vector<float> f(mFreqs.begin(), mFreqs.end());
    return f;
  }

  // 音频线程: 逐样本执行 4 阶 TPT SVF 滤波并在 1024 样本区间内提取峰值
  void ProcessBlock(sample **inputs, int nFrames, int ctrlTag = kNoTag, int nChans = MAXNC,
                    int chanOffset = 0) {
    CheckRebuild();
    const int nCh = std::min(nChans, MAXNC);
    const int nb = (int)mBands.size();
    if (nb <= 0)
      return;

    for (int s = 0; s < nFrames; ++s) {
      for (int c = 0; c < nCh; ++c) {
        const float inSample = (float)inputs[chanOffset + c][s];
        BandState *states = mStates[c].data();

        for (int b = 0; b < nb; ++b) {
          const BandCoef &coef = mBands[b];
          BandState &st = states[b];

          // 级联第 1 级 2 阶 TPT SVF 带通
          const float v3_1 = inSample - st.ic2_1;
          const float v1_1 = coef.a1 * st.ic1_1 + coef.a2 * v3_1;
          const float v2_1 = st.ic2_1 + coef.a2 * st.ic1_1 + coef.a3 * v3_1;
          st.ic1_1 = 2.f * v1_1 - st.ic1_1;
          st.ic2_1 = 2.f * v2_1 - st.ic2_1;

          // 级联第 2 级 2 阶 TPT SVF 带通 (输入为第 1 级的带通输出 v1_1)
          const float v3_2 = v1_1 - st.ic2_2;
          const float v1_2 = coef.a1 * st.ic1_2 + coef.a2 * v3_2;
          const float v2_2 = st.ic2_2 + coef.a2 * st.ic1_2 + coef.a3 * v3_2;
          st.ic1_2 = 2.f * v1_2 - st.ic1_2;
          st.ic2_2 = 2.f * v2_2 - st.ic2_2;

          // 去非规格化微小浮点数保护 (防止 CPU 性能惩罚)
          if (std::abs(st.ic1_1) < 1e-30f) {
            st.ic1_1 = 0.f; st.ic2_1 = 0.f;
            st.ic1_2 = 0.f; st.ic2_2 = 0.f;
          }

          // 归一化幅度输出并记录 Hop 内峰值
          const float mag = std::abs(v1_2) * coef.kb;
          if (mag > st.pk)
            st.pk = mag;
        }
      }

      if (++mBufCount == kHop) {
        Data d{ctrlTag, nCh, chanOffset};
        for (int c = 0; c < nCh; ++c) {
          BandState *states = mStates[c].data();
          for (int b = 0; b < nb; ++b) {
            d.vals[c][b] = states[b].pk;
            states[b].pk = 0.f; // 重置下一 hop 的峰值累加器
          }
          for (int b = nb; b < MAX_BANDS; ++b)
            d.vals[c][b] = 0.f;
        }
        Base::PushData(d);
        mBufCount = 0;
      }
    }
  }

protected:
  void PrepareDataForUI(Data &d) override {
    // 数据已在音频线程完成归一化峰值提取，此处直接透传
  }

private:
  struct BandCoef {
    float a1, a2, a3; // TPT SVF 系数 (级联两级结构完全相同)
    float kb;         // 归一化增益因子 (1/Q)^2
  };

  struct BandState {
    float ic1_1 = 0.f, ic2_1 = 0.f; // 级联第 1 级状态
    float ic1_2 = 0.f, ic2_2 = 0.f; // 级联第 2 级状态
    float pk = 0.f;                 // Hop 峰值记录
  };

  void RebuildBands() {
    mBands.clear();
    mFreqs.clear();
    const double fs = std::max(mSampleRate, 1.0);
    const double lfWidth = (mLfMode == 0) ? 40.0 : (mLfMode == 1) ? 20.0 : 10.0;

    // 1. 低频段 (< 250 Hz): 固定等带宽线性递增
    double fc = kFreqLo;
    while (fc < 250.0) {
      const double Q = std::max(0.5, fc / lfWidth);
      AddBand(fc, Q, fs);
      fc += lfWidth;
    }

    // 2. 高频段 (≥ 250 Hz): 恒定 Q=10 等比递推 (1.1x)
    fc = 250.0;
    const double qHigh = 10.0;
    const double nyqLimit = std::min(kFreqHi, 0.45 * fs);
    while (fc <= nyqLimit) {
      AddBand(fc, qHigh, fs);
      fc *= (1.0 + 1.0 / qHigh);
    }

    // 复位各通道状态
    for (int c = 0; c < MAXNC; ++c)
      mStates[c].assign(mBands.size(), BandState{});
    mBufCount = 0;
  }

  void AddBand(double fc, double Q, double fs) {
    constexpr double kPi = 3.14159265358979323846;
    const double g = std::tan(kPi * fc / fs);
    const double k = 1.0 / Q;
    const double a1 = 1.0 / (1.0 + g * (g + k));
    const double a2 = g * a1;
    const double a3 = g * a2;
    const double kb = 1.0 / (Q * Q); // 4 阶带通在中心频点的固有增益为 Q^2，乘 (1/Q)^2 严格归一到 0 dB

    BandCoef coef;
    coef.a1 = (float)a1;
    coef.a2 = (float)a2;
    coef.a3 = (float)a3;
    coef.kb = (float)kb;

    mBands.push_back(coef);
    mFreqs.push_back(fc);
  }

  double mSampleRate = 48000.0;
  int mLfMode = 0; // 0=40Hz, 1=20Hz, 2=10Hz
  std::atomic<bool> mNeedRebuild{false};
  std::mutex mMutex;

  std::vector<BandCoef> mBands;
  std::vector<double> mFreqs;
  std::array<std::vector<BandState>, MAXNC> mStates;
  int mBufCount = 0;
};

END_IPLUG_NAMESPACE
