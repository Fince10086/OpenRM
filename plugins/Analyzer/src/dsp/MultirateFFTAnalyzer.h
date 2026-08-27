#pragma once

// MultirateFFTAnalyzer — 多速率八度子带 FFT 频谱分析引擎 (MR-FFT)

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
// 半带 ×2 抽取器 (跨帧保持滤波状态)。系数为 101 抽头 4 项 Blackman-Harris 窗半带 (截止 π/2, DC 增益 1):
// 阻带跌落 >90 dB, 杜绝层边界强单音穿透抽取器折返到下层的"假频谱峰" (旧 17 抽头 Hamming 阻带
// 边缘仅 ~-15..-53 dB)。偶数序 (除中心) 抽头严格为零。要求 nin 为偶数。
struct HalfbandDec2 {
  static constexpr int kN = 101;
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
      const double theta = 2.0 * kPi * i / (kN - 1); // 4 项 Blackman-Harris: 旁瓣 -92 dB (Hamming 仅 -53 dB)
      v *= 0.35875 - 0.48829 * std::cos(theta)
                   + 0.14128 * std::cos(2.0 * theta)
                   - 0.01168 * std::cos(3.0 * theta);
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

// MAX_BANDS 与其他引擎的包尺寸保持一致
template <int MAXNC = 3, int QUEUE_SIZE = 64, int MAX_BANDS = 8192>
class MultirateFFTAnalyzer : public ISender<MAXNC, QUEUE_SIZE, std::array<float, MAX_BANDS>> {
public:
  using TDataPacket = std::array<float, MAX_BANDS>;
  using Data = ISenderData<MAXNC, TDataPacket>;
  using Base = ISender<MAXNC, QUEUE_SIZE, TDataPacket>;

  static constexpr double kFreqLo = 20.0;
  static constexpr double kFreqHi = 20000.0;
  static constexpr int kHop = 1024;
  static constexpr int kMaxLayers = 10;
  static constexpr double kGuard = 0.38;

  MultirateFFTAnalyzer() {
    WDL_fft_init();
    for (int c = 0; c < MAXNC; ++c)
      mPending[c].assign(kHop, 0.f);
    InitLayerWindows();
    RebuildBands();
  }

  bool SetBpo(int bpo) {
    const int b = (bpo == 12) ? 12 : 24;
    if (b != mBpo) {
      mBpo = b;
      mNeedRebuild.store(true, std::memory_order_release);
      return true;
    }
    return false;
  }

  void SetSampleRate(double sr) {
    if (std::abs(mSampleRate - sr) > 0.1) {
      mSampleRate = sr;
      mNeedRebuild.store(true, std::memory_order_release);
    }
  }

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

      // 逐层执行加窗实数 FFT
      for (int l = 0; l < kMaxLayers; ++l) {
        const int nFft = mFftSize[l];
        const int nNew = kHop >> l;
        float *hist = mFftHist[c][l].data();

        // 更新历史缓冲
        if (nNew >= nFft) {
          std::memcpy(hist, mLayers[c][l].data() + (nNew - nFft), nFft * sizeof(float));
        } else {
          std::memmove(hist, hist + nNew, (nFft - nNew) * sizeof(float));
          std::memcpy(hist + (nFft - nNew), mLayers[c][l].data(), nNew * sizeof(float));
        }

        // 加窗并执行 FFT
        WDL_FFT_COMPLEX *fb = mFftBuf[c][l].data();
        const float *win = mWindows[l].data();
        for (int i = 0; i < nFft; ++i) {
          fb[i].re = hist[i] * win[i];
          fb[i].im = 0.0f;
        }

        WDL_fft(fb, nFft, false);

        // 计算幅度谱
        float *mags = mMagBuf[c][l].data();
        const int nBins = nFft / 2;
        const float norm = mFftScaling[l];
        for (int i = 0; i < nBins; ++i) {
          const int si = WDL_fft_permute(nFft, i);
          const float re = fb[si].re, im = fb[si].im;
          mags[i] = std::sqrt(re * re + im * im) * norm;
        }
      }

      // 提取频带幅度并应用主瓣校正
      for (int b = 0; b < nb; ++b) {
        const Band &bd = mBands[b];
        const float rawMag = mMagBuf[c][bd.layer][bd.kPeak];
        d.vals[c][b] = rawMag * bd.corrFactor;
      }

      for (int b = nb; b < MAX_BANDS; ++b)
        d.vals[c][b] = 0.f;
    }
  }

private:
  struct Band {
    int layer;         // 所属金字塔层级 (0..9)
    int kPeak;         // FFT Bin 序号
    float corrFactor;  // 窗函数反折损补偿系数
  };

  void InitLayerWindows() {
    constexpr double kPi = 3.14159265358979323846;
    for (int l = 0; l < kMaxLayers; ++l) {
      const int sz = (l <= 4) ? (1024 >> l) : 64;
      mFftSize[l] = sz;
      mWindows[l].resize(sz);
      double sum = 0.0;
      for (int i = 0; i < sz; ++i) {
        // Hamming 窗
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

  void RebuildBands() {
    mBands.clear();
    mFreqs.clear();
    const double fs = std::max(mSampleRate, 1.0);
    const double safeLimit = kGuard * fs;

    for (int k = 0;; ++k) {
      const double fc = kFreqLo * std::pow(2.0, (double)k / mBpo);
      if (fc > kFreqHi)
        break;

      // 层分配
      int layer = 0;
      if (fc < safeLimit) {
        layer = (int)std::floor(std::log2(safeLimit / std::max(fc, 1.0)));
        layer = std::clamp(layer, 0, kMaxLayers - 1);
      }

      // 计算对应层 FFT 的谱线位置
      const double layerFs = fs / (double)(1 << layer);
      const int nFft = mFftSize[layer];
      const double kFrac = fc * (double)nFft / layerFs;
      const int kPeak = std::clamp((int)std::round(kFrac), 0, nFft / 2 - 1);

      // Hamming 窗反折损补偿
      const double p = std::clamp(kFrac - (double)kPeak, -0.5, 0.5);
      constexpr double kPi = 3.14159265358979323846;
      double corr = 1.0;
      if (std::abs(p) > 1e-4) {
        const double sincP = std::sin(kPi * p) / (kPi * p);
        corr = (0.54 * (1.0 - p * p)) / (sincP * (0.54 - 0.08 * p * p));
      }

      Band bd;
      bd.layer = layer;
      bd.kPeak = kPeak;
      bd.corrFactor = (float)corr;

      mBands.push_back(bd);
      mFreqs.push_back(fc);
    }

    for (int c = 0; c < MAXNC; ++c) {
      for (int l = 0; l < kMaxLayers - 1; ++l)
        mDecim[c][l].Reset();
      for (int l = 0; l < kMaxLayers; ++l) {
        mLayers[c][l].assign(kHop >> l, 0.f);
        mFftHist[c][l].assign(mFftSize[l], 0.f);
      }
    }
  }

  int mBpo = 24; // 12 或 24
  double mSampleRate = 48000.0;
  int mChanTri = 0; // 0=LR, 1=PWR, 2=SUM
  std::atomic<bool> mNeedRebuild{false};

  std::vector<Band> mBands;
  std::vector<double> mFreqs;

  std::array<int, kMaxLayers> mFftSize{};
  std::array<float, kMaxLayers> mFftScaling{};
  std::array<std::vector<float>, kMaxLayers> mWindows;

  std::array<std::vector<float>, MAXNC> mPending;
  int mBufCount = 0;

  std::array<std::array<detail::HalfbandDec2, kMaxLayers - 1>, MAXNC> mDecim;
  std::array<std::array<std::vector<float>, kMaxLayers>, MAXNC> mLayers;
  std::array<std::array<std::vector<float>, kMaxLayers>, MAXNC> mFftHist;
  std::array<std::array<std::vector<WDL_FFT_COMPLEX>, kMaxLayers>, MAXNC> mFftBuf;
  std::array<std::array<std::vector<float>, kMaxLayers>, MAXNC> mMagBuf;
};

END_IPLUG_NAMESPACE
