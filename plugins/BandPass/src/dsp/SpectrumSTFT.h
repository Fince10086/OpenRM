#pragma once

// SpectrumSTFT — 基于短时傅里叶变换 (STFT) 的频谱分析引擎 (BandPass 定制版)
// 算法与 Analyzer 的 STFT 引擎同源: 4 阶 Blackman-Harris 窗 (旁瓣 -92 dB) +
// WDL 实数 FFT (计算量约为同尺寸复数 FFT 的一半)。窗函数固定, 不提供选项。

#include "ISender.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

BEGIN_IPLUG_NAMESPACE

template <int MAXNC = 2, int QUEUE_SIZE = 64, int MAX_FFT_SIZE = 4096>
class SpectrumSTFT : public ISender<MAXNC, QUEUE_SIZE, std::array<float, MAX_FFT_SIZE>> {
public:
  using TDataPacket = std::array<float, MAX_FFT_SIZE>;
  using Data = ISenderData<MAXNC, TDataPacket>;
  using Base = ISender<MAXNC, QUEUE_SIZE, TDataPacket>;

  SpectrumSTFT(int fftSize = 4096, int overlap = 4) {
    WDL_fft_init();
    SetFFTSizeAndOverlap(fftSize, overlap);
  }

  void SetFFTSizeAndOverlap(int fftSize, int overlap) {
    mFFTSize = std::clamp(fftSize, 64, MAX_FFT_SIZE);
    mOverlap = std::max(overlap, 1);
    mHop = mFFTSize / mOverlap;
    mNumBins = mFFTSize / 2;
    RebuildWindow();

    for (auto &h : mHistory)
      h.fill(0.f);
    mBufCount = 0;
  }

  void SetFFTSize(int fftSize) { SetFFTSizeAndOverlap(fftSize, mOverlap); }

  int GetFFTSize() const { return mFFTSize; }
  int GetOverlap() const { return mOverlap; }

  void ProcessBlock(sample **inputs, int nFrames, int ctrlTag = kNoTag, int nChans = MAXNC, int chanOffset = 0) {
    const int nCh = std::min(nChans, MAXNC);
    for (int s = 0; s < nFrames; ++s) {
      for (int c = 0; c < nCh; ++c)
        mPending[c][mBufCount] = static_cast<float>(inputs[chanOffset + c][s]);
      if (++mBufCount == mHop) {
        Data d{ctrlTag, nCh, chanOffset};
        for (int c = 0; c < nCh; ++c)
          std::copy(mPending[c].begin(), mPending[c].begin() + mHop, d.vals[c].begin());
        Base::PushData(d);
        mBufCount = 0;
      }
    }
  }

protected:
  void PrepareDataForUI(Data &d) override {
    const int nCh = std::min(d.nChans, MAXNC);

    for (int c = d.chanOffset; c < d.chanOffset + nCh; ++c) {
      std::memmove(mHistory[c].data(), mHistory[c].data() + mHop, (mFFTSize - mHop) * sizeof(float));
      std::memcpy(mHistory[c].data() + mFFTSize - mHop, d.vals[c].data(), mHop * sizeof(float));

      WDL_FFT_REAL *rb = mRealBuf[c].data();
      for (int i = 0; i < mFFTSize; ++i) {
        rb[i] = mHistory[c][i] * mWindow[i];
      }
      WDL_real_fft(rb, mFFTSize, 0);

      const WDL_FFT_COMPLEX *comp = reinterpret_cast<const WDL_FFT_COMPLEX *>(rb);
      const int halfSize = mFFTSize / 2;
      // WDL_real_fft 输出为单边谱约定: 各 bin 是真实 DFT 值的 2 倍, 需除 2 归一。
      // 标定不变量: bin 中心正弦幅度 A 读 A/√2, m = sqrt(2·|X|²/Σw²) = |comp|²·0.5/mScaling
      const float invScalingHalf = 0.5f / mScaling;

      // DC (bin 0)
      d.vals[c][0] = std::sqrt((comp[0].re * comp[0].re) * invScalingHalf);

      // 正频段 (bin 1 .. mNumBins - 1)
      for (int i = 1; i < mNumBins; ++i) {
        const int si = WDL_fft_permute(halfSize, i);
        const float re = comp[si].re, im = comp[si].im;
        d.vals[c][i] = std::sqrt((re * re + im * im) * invScalingHalf);
      }
      for (int i = mNumBins; i < MAX_FFT_SIZE; ++i)
        d.vals[c][i] = 0.0f;
    }
  }

private:
  // 4-term Blackman-Harris 窗: 旁瓣抑制达 -92 dB, 干/湿叠加显示时杜绝横向泄漏
  void RebuildWindow() {
    const float M = static_cast<float>(mFFTSize - 1);
    constexpr float a0 = 0.35875f;
    constexpr float a1 = 0.48829f;
    constexpr float a2 = 0.14128f;
    constexpr float a3 = 0.01168f;
    double sum = 0.0;
    for (int i = 0; i < mFFTSize; ++i) {
      const float theta = 2.0f * PI * i / M;
      mWindow[i] = a0 - a1 * std::cos(theta) + a2 * std::cos(2.0f * theta) - a3 * std::cos(3.0f * theta);
      sum += mWindow[i];
    }
    mScaling = static_cast<float>(sum * sum);
  }

  int mFFTSize = 4096;
  int mOverlap = 4;
  int mHop = 1024;
  int mNumBins = 2048;
  int mBufCount = 0;
  float mScaling = 0.f;
  std::array<float, MAX_FFT_SIZE> mWindow{};
  std::array<std::array<float, MAX_FFT_SIZE>, MAXNC> mHistory{};
  std::array<std::array<WDL_FFT_REAL, MAX_FFT_SIZE>, MAXNC> mRealBuf{};
  std::array<std::array<float, MAX_FFT_SIZE>, MAXNC> mPending{};
};

END_IPLUG_NAMESPACE
