#pragma once

// SpectrumSTFT — 基于短时傅里叶变换 (STFT) 的标准频谱分析引擎

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

    const float M = static_cast<float>(mFFTSize - 1);
    double sum = 0.0;
    for (int i = 0; i < mFFTSize; ++i) {
      mWindow[i] = 0.5f * (1.0f - std::cos(PI * 2.0f * i / M));
      sum += mWindow[i];
    }
    mScaling = static_cast<float>(sum * sum);

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

      WDL_FFT_COMPLEX *fb = mFFTBuf[c].data();
      for (int i = 0; i < mFFTSize; ++i) {
        fb[i].re = mHistory[c][i] * mWindow[i];
        fb[i].im = 0.0f;
      }
      WDL_fft(fb, mFFTSize, false);

      for (int i = 0; i < mNumBins; ++i) {
        const int si = WDL_fft_permute(mFFTSize, i);
        const float re = fb[si].re, im = fb[si].im;
        d.vals[c][i] = std::sqrt(2.0f * (re * re + im * im) / mScaling);
      }
      for (int i = mNumBins; i < MAX_FFT_SIZE; ++i)
        d.vals[c][i] = 0.0f;
    }
  }

private:
  int mFFTSize = 4096;
  int mOverlap = 4;
  int mHop = 1024;
  int mNumBins = 2048;
  int mBufCount = 0;
  float mScaling = 0.f;
  std::array<float, MAX_FFT_SIZE> mWindow{};
  std::array<std::array<float, MAX_FFT_SIZE>, MAXNC> mHistory{};
  std::array<std::array<WDL_FFT_COMPLEX, MAX_FFT_SIZE>, MAXNC> mFFTBuf{};
  std::array<std::array<float, MAX_FFT_SIZE>, MAXNC> mPending{};
};

END_IPLUG_NAMESPACE
