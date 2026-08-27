#pragma once

// PAZAnalyzer — 心理声学临界频带频谱分析引擎 (支持 IIR 与 FFT 双算法)

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

template <int MAXNC = 3, int QUEUE_SIZE = 64, int MAX_BANDS = 8192>
class PAZAnalyzer : public ISender<MAXNC, QUEUE_SIZE, std::array<float, MAX_BANDS>> {
public:
  using TDataPacket = std::array<float, MAX_BANDS>;
  using Data = ISenderData<MAXNC, TDataPacket>;
  using Base = ISender<MAXNC, QUEUE_SIZE, TDataPacket>;

  static constexpr int kHop = 1024;
  static constexpr int kMaxLayers = 10;

  PAZAnalyzer() {
    WDL_fft_init();
    for (int c = 0; c < MAXNC; ++c)
      mPending[c].assign(kHop, 0.f);
    InitLayerWindows();
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

  // 设置算法模式 (0: IIR, 1: FFT)
  void SetAlgo(int algo) {
    mAlgo = std::clamp(algo, 0, 1);
  }
  int GetAlgo() const { return mAlgo; }

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

#ifdef STANDALONE_TEST
  void TestProcessHop(Data &d) {
    PrepareDataForUI(d);
  }
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
      float *l0 = mLayers[c][0].data();
      std::copy(d.vals[c].begin(), d.vals[c].begin() + kHop, l0);
      int nin = kHop;
      for (int l = 0; l < kMaxLayers - 1; ++l) {
        nin = mDecim[c][l].Process(mLayers[c][l].data(), nin, mLayers[c][l + 1].data());
      }

      if (mAlgo == 1) {
        // FFT 算法
        for (int l = 0; l < kMaxLayers; ++l) {
          const int nFft = mFftSize[l];
          const int nNew = kHop >> l;
          float *hist = mFftHist[c][l].data();

          if (nNew >= nFft) {
            std::memcpy(hist, mLayers[c][l].data() + (nNew - nFft), nFft * sizeof(float));
          } else {
            std::memmove(hist, hist + nNew, (nFft - nNew) * sizeof(float));
            std::memcpy(hist + (nFft - nNew), mLayers[c][l].data(), nNew * sizeof(float));
          }

          WDL_FFT_COMPLEX *fb = mFftBuf[c][l].data();
          const float *win = mWindows[l].data();
          for (int i = 0; i < nFft; ++i) {
            fb[i].re = hist[i] * win[i];
            fb[i].im = 0.0f;
          }

          WDL_fft(fb, nFft, false);

          float *mags = mMagBuf[c][l].data();
          const int nBins = nFft / 2;
          const float norm = mFftScaling[l];
          for (int i = 0; i < nBins; ++i) {
            const int si = WDL_fft_permute(nFft, i);
            const float re = fb[si].re, im = fb[si].im;
            mags[i] = std::sqrt(re * re + im * im) * norm;
          }
        }

        for (int b = 0; b < nb; ++b) {
          const FftBandInfo &fbd = mFftBands[b];
          const float rawMag = mMagBuf[c][fbd.layer][fbd.kPeak];
          d.vals[c][b] = rawMag * fbd.corrFactor;
        }
      } else {
        // IIR 算法 (4 阶 TPT SVF 滤波)
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

            // 级联第 1 级 2 阶带通
            const float v3_1 = inSample - ic2_1;
            const float v1_1 = coef.a1 * ic1_1 + coef.a2 * v3_1;
            const float v2_1 = ic2_1 + coef.a2 * ic1_1 + coef.a3 * v3_1;
            ic1_1 = 2.f * v1_1 - ic1_1;
            ic2_1 = 2.f * v2_1 - ic2_1;

            // 级联第 2 级 2 阶带通
            const float v3_2 = v1_1 - ic2_2;
            const float v1_2 = coef.a1 * ic1_2 + coef.a2 * v3_2;
            const float v2_2 = ic2_2 + coef.a2 * ic1_2 + coef.a3 * v3_2;
            ic1_2 = 2.f * v1_2 - ic1_2;
            ic2_2 = 2.f * v2_2 - ic2_2;

            const float mag = std::abs(v1_2) * coef.kb;
            if (mag > pk)
              pk = mag;
          }

          if (std::abs(ic1_1) < 1e-30f) {
            ic1_1 = ic2_1 = ic1_2 = ic2_2 = 0.f;
          }

          st.ic1_1 = ic1_1; st.ic2_1 = ic2_1;
          st.ic1_2 = ic1_2; st.ic2_2 = ic2_2;

          d.vals[c][b] = pk;
        }
      }

      for (int b = nb; b < MAX_BANDS; ++b)
        d.vals[c][b] = 0.f;
    }
  }

private:
  struct BandCoef {
    float a1, a2, a3;
    float kb;
    int layer;
  };

  struct BandState {
    float ic1_1 = 0.f, ic2_1 = 0.f;
    float ic1_2 = 0.f, ic2_2 = 0.f;
  };

  struct FftBandInfo {
    int layer;
    int kPeak;
    float corrFactor;
  };

  void InitLayerWindows() {
    constexpr double kPi = 3.14159265358979323846;
    for (int l = 0; l < kMaxLayers; ++l) {
      const int sz = (l <= 4) ? (1024 >> l) : 64;
      mFftSize[l] = sz;
      mWindows[l].resize(sz);
      double sum = 0.0;
      for (int i = 0; i < sz; ++i) {
        const double w = 0.54 - 0.46 * std::cos(2.0 * kPi * i / (sz - 1));
        mWindows[l][i] = (float)w;
        sum += w;
      }
      mFftScaling[l] = (float)(2.0 / sum);

      for (int c = 0; c < MAXNC; ++c) {
        mFftHist[c][l].assign(sz, 0.f);
        mFftBuf[c][l].resize(sz);
        mMagBuf[c][l].assign(sz / 2, 0.f);
      }
    }
  }

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
    mFftBands.clear();
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

      // 带宽与 Q 值
      double bw;
      if (i == 0)
        bw = (srcFreqs[1] - srcFreqs[0]) * scale;
      else if (i == numSrc - 1)
        bw = (srcFreqs[numSrc - 1] - srcFreqs[numSrc - 2]) * scale;
      else
        bw = 0.5 * (srcFreqs[i + 1] - srcFreqs[i - 1]) * scale;

      const double Q = std::clamp(fc / std::max(bw, 1.0), 0.707, 15.0);

      // 分配金字塔层级 L
      static constexpr double kGuard = 0.38;
      const double safeLimit = kGuard * fs;
      int layer = 0;
      if (fc < safeLimit) {
        layer = (int)std::floor(std::log2(safeLimit / std::max(fc, 1.0)));
        layer = std::clamp(layer, 0, kMaxLayers - 1);
      }

      // IIR 系数
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

      // FFT 频点与校正系数
      const int nFft = mFftSize[layer];
      const double kFrac = fc * (double)nFft / layerFs;
      const int kPeak = std::clamp((int)std::round(kFrac), 0, nFft / 2 - 1);
      const double p = std::clamp(kFrac - (double)kPeak, -0.5, 0.5);
      double corr = 1.0;
      if (std::abs(p) > 1e-4) {
        const double sincP = std::sin(kPi * p) / (kPi * p);
        corr = (0.54 * (1.0 - p * p)) / (sincP * (0.54 - 0.08 * p * p));
      }

      FftBandInfo fbd;
      fbd.layer = layer;
      fbd.kPeak = kPeak;
      fbd.corrFactor = (float)corr;
      mFftBands.push_back(fbd);
    }

    for (int c = 0; c < MAXNC; ++c) {
      mStates[c].assign(mBands.size(), BandState{});
      for (int l = 0; l < kMaxLayers - 1; ++l)
        mDecim[c][l].Reset();
      for (int l = 0; l < kMaxLayers; ++l) {
        mLayers[c][l].assign(kHop >> l, 0.f);
        mFftHist[c][l].assign(mFftSize[l], 0.f);
      }
    }
  }

  int mAlgo = 0; // 0: IIR, 1: FFT
  double mSampleRate = 48000.0;
  int mLfMode = 0; // 0=40Hz, 1=20Hz, 2=10Hz
  int mChanTri = 0; // 0=LR, 1=PWR, 2=SUM
  std::atomic<bool> mNeedRebuild{false};

  std::vector<BandCoef> mBands;
  std::vector<FftBandInfo> mFftBands;
  std::vector<double> mFreqs;
  std::array<std::vector<BandState>, MAXNC> mStates;
  std::array<std::vector<float>, MAXNC> mPending;
  int mBufCount = 0;

  std::array<int, kMaxLayers> mFftSize{};
  std::array<float, kMaxLayers> mFftScaling{};
  std::array<std::vector<float>, kMaxLayers> mWindows;

  std::array<std::array<detail::HalfbandDec2, kMaxLayers - 1>, MAXNC> mDecim;
  std::array<std::array<std::vector<float>, kMaxLayers>, MAXNC> mLayers;
  std::array<std::array<std::vector<float>, kMaxLayers>, MAXNC> mFftHist;
  std::array<std::array<std::vector<WDL_FFT_COMPLEX>, kMaxLayers>, MAXNC> mFftBuf;
  std::array<std::array<std::vector<float>, kMaxLayers>, MAXNC> mMagBuf;
};

END_IPLUG_NAMESPACE
