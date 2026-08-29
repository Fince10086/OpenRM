#pragma once

// RTAAnalyzer — 模拟硬件风格 IIR 带通滤波器组实时频谱分析引擎 (Filter-Bank RTA)
//
// 核心原理:
// 1. 遵循 ANSI S1.11 / IEC 61260 标准分数倍频程划分 (1/3 Oct, 1/4 Oct, 1/6 Oct)
// 2. 逐频带配置级联二阶/四阶带通 IIR 滤波器 (Direct Form II Transposed, 0 dB 峰值归一)
// 3. 逐采样点流式滤波 + 真 RMS 能量检波 (无 FFT 块时延与窗函数涂抹, 呈现经典硬件 RTA 物理响应)

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
  static constexpr double kPi = 3.14159265358979323846;

  enum EOctaveMode {
    kOctave1_3 = 0, // 1/3 Octave (~31 bands, Q ≈ 4.32)
    kOctave1_4 = 1, // 1/4 Octave (~41 bands, Q ≈ 5.77)
    kOctave1_6 = 2, // 1/6 Octave (~61 bands, Q ≈ 8.65)
    kNumOctaveModes = 3
  };

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

  // Freeze (冻结) 支持: 复位分析侧运行态 (所有频带滤波器历史状态清零)
  void ResetRuntimeState() {
    for (int c = 0; c < MAXNC; ++c) {
      for (auto &st : mStates[c]) {
        st.z1_1 = st.z2_1 = 0.f;
        st.z1_2 = st.z2_2 = 0.f;
      }
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

    constexpr float invHop = 1.0f / (float)kHop;
    // 归一化系数: 正弦波幅度为 1 时，均方值为 0.5，乘 2 开方后得峰值幅度 1.0 (0 dBFS)
    constexpr float rmsNorm = 2.0f * invHop;

    for (int c = 0; c < nCh; ++c) {
      if ((c == 0 && !needL) || (c == 1 && !needR) || (c == 2 && !needSum)) {
        for (int b = 0; b < MAX_BANDS; ++b)
          d.vals[c][b] = 0.f;
        continue;
      }

      alignas(16) float raw[kHop];
      std::memcpy(raw, d.vals[c].data(), kHop * sizeof(float));

      for (int b = 0; b < nb; ++b) {
        const BandFilter &filter = mBands[b];
        BandState &st = mStates[c][b];

        const float cb0 = filter.b0;
        const float ca1 = filter.a1;
        const float ca2 = filter.a2;

        float z1_1 = st.z1_1, z2_1 = st.z2_1;
        float z1_2 = st.z1_2, z2_2 = st.z2_2;

        float sumSq = 0.f;

        // 4阶双二阶级联 (2级级联以获得 24 dB/oct 锐利衰减与标准 ANSI 临带隔离度)
        for (int n = 0; n < kHop; ++n) {
          const float x = raw[n];

          // 级联 1
          const float y1 = cb0 * x + z1_1;
          z1_1 = -ca1 * y1 + z2_1;
          z2_1 = -cb0 * x - ca2 * y1;

          // 级联 2
          const float y2 = cb0 * y1 + z1_2;
          z1_2 = -ca1 * y2 + z2_2;
          z2_2 = -cb0 * y1 - ca2 * y2;

          sumSq += y2 * y2;
        }

        // 抗 Denormal 保护
        if (std::abs(z1_1) < 1e-25f) z1_1 = 0.f;
        if (std::abs(z2_1) < 1e-25f) z2_1 = 0.f;
        if (std::abs(z1_2) < 1e-25f) z1_2 = 0.f;
        if (std::abs(z2_2) < 1e-25f) z2_2 = 0.f;

        st.z1_1 = z1_1;
        st.z2_1 = z2_1;
        st.z1_2 = z1_2;
        st.z2_2 = z2_2;

        // 幅度检波: 真 RMS 能量开方
        d.vals[c][b] = std::sqrt(sumSq * rmsNorm);
      }

      for (int b = nb; b < MAX_BANDS; ++b)
        d.vals[c][b] = 0.f;
    }
  }

private:
  struct BandFilter {
    float b0 = 0.f;
    float a1 = 0.f;
    float a2 = 0.f;
  };

  struct BandState {
    float z1_1 = 0.f, z2_1 = 0.f; // 级联 1 状态
    float z1_2 = 0.f, z2_2 = 0.f; // 级联 2 状态
  };

  void RebuildBands() {
    mBands.clear();
    mFreqs.clear();

    const double fs = std::max(mSampleRate, 1.0);
    const double nyqLimit = 0.485 * fs;

    int bpo = 3;
    if (mOctaveMode == kOctave1_4)
      bpo = 4;
    else if (mOctaveMode == kOctave1_6)
      bpo = 6;

    // 单级 Q 值调整: 使两级级联后的总 -3dB 带宽与标准分数倍频程 Q 匹配
    // Q_stage = Q_total * sqrt(sqrt(2) - 1) ≈ 0.6435942529 * Q_total
    const double qTotal = 1.0 / (std::pow(2.0, 1.0 / (2.0 * (double)bpo)) -
                                std::pow(2.0, -1.0 / (2.0 * (double)bpo)));
    const double qStage = qTotal * 0.6435942529055826;

    // 基准 1000 Hz 几何中心网格
    // 覆盖范围: ~16 Hz .. 22 kHz (并在 nyqLimit 处截断)
    int kMin = -6 * bpo; // 1000 * 2^-6 ≈ 15.6 Hz
    int kMax = 5 * bpo;  // 1000 * 2^5 = 32000 Hz

    for (int k = kMin; k <= kMax; ++k) {
      const double fc = 1000.0 * std::pow(2.0, (double)k / (double)bpo);
      if (fc < 16.0 || fc >= nyqLimit)
        continue;

      // 双二阶带通滤波器系数计算 (Audio EQ Cookbook: 0 dB 峰值增益带通)
      const double w0 = 2.0 * kPi * fc / fs;
      const double sinW = std::sin(w0);
      const double cosW = std::cos(w0);
      const double alpha = sinW / (2.0 * qStage);

      const double a0 = 1.0 + alpha;
      BandFilter filter;
      filter.b0 = (float)(alpha / a0);
      filter.a1 = (float)(-2.0 * cosW / a0);
      filter.a2 = (float)((1.0 - alpha) / a0);

      mBands.push_back(filter);
      mFreqs.push_back(fc);
    }

    for (int c = 0; c < MAXNC; ++c) {
      mStates[c].assign(mBands.size(), BandState{});
    }

    ResetRuntimeState();
  }

  double mSampleRate = 48000.0;
  int mOctaveMode = kOctave1_3; // 0=1/3, 1=1/4, 2=1/6
  int mChanTri = 0;             // 0=LR, 1=PWR, 2=SUM
  std::atomic<bool> mNeedRebuild{false};

  std::vector<BandFilter> mBands;
  std::vector<double> mFreqs;
  std::array<std::vector<BandState>, MAXNC> mStates;

  std::array<std::vector<float>, MAXNC> mPending;
  int mBufCount = 0;
};

END_IPLUG_NAMESPACE
