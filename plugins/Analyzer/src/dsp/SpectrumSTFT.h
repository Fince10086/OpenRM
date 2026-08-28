#pragma once

// SpectrumSTFT — 基于短时傅里叶变换 (STFT) 的标准频谱分析引擎

#include "ISender.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

BEGIN_IPLUG_NAMESPACE

template <int MAXNC = 3, int QUEUE_SIZE = 64, int MAX_FFT_SIZE = 8192>
class SpectrumSTFT : public ISender<MAXNC, QUEUE_SIZE, std::array<float, MAX_FFT_SIZE>> {
public:
  using TDataPacket = std::array<float, MAX_FFT_SIZE>;
  using Data = ISenderData<MAXNC, TDataPacket>;
  using Base = ISender<MAXNC, QUEUE_SIZE, TDataPacket>;

  enum EWindowType { kWindowHann = 0, kWindowBH4 = 1, kNumWindowTypes = 2 };

  SpectrumSTFT(int fftSize = 4096, int overlap = 4, int windowType = 0) {
    WDL_fft_init();
    SetFFTSizeAndOverlap(fftSize, overlap, windowType);
  }

  void SetFFTSizeAndOverlap(int fftSize, int overlap, int windowType = -1) {
    mFFTSize = std::clamp(fftSize, 64, MAX_FFT_SIZE);
    mOverlap = std::max(overlap, 1);
    mHop = mFFTSize / mOverlap;
    mNumBins = mFFTSize / 2;
    if (windowType >= 0)
      mWindowType = std::clamp(windowType, 0, 1);
    RebuildWindow();

    for (auto &h : mHistory)
      h.fill(0.f);
    mBufCount = 0;
  }

  void SetWindowType(int windowType) {
    const int newType = std::clamp(windowType, 0, 1);
    if (newType != mWindowType) {
      mWindowType = newType;
      RebuildWindow();
    }
  }

  int GetWindowType() const { return mWindowType; }

  void RebuildWindow() {
    const float M = static_cast<float>(mFFTSize - 1);
    double sum = 0.0;
    if (mWindowType == 1) {
      // 4-term Blackman-Harris 窗: 旁瓣抑制达 -92 dB
      constexpr float a0 = 0.35875f;
      constexpr float a1 = 0.48829f;
      constexpr float a2 = 0.14128f;
      constexpr float a3 = 0.01168f;
      for (int i = 0; i < mFFTSize; ++i) {
        const float theta = 2.0f * PI * i / M;
        mWindow[i] = a0 - a1 * std::cos(theta) + a2 * std::cos(2.0f * theta) - a3 * std::cos(3.0f * theta);
        sum += mWindow[i];
      }
    } else {
      // 标准 Hann 窗: 主瓣较窄, 旁瓣 -31.5 dB
      for (int i = 0; i < mFFTSize; ++i) {
        mWindow[i] = 0.5f * (1.0f - std::cos(2.0f * PI * i / M));
        sum += mWindow[i];
      }
    }
    mScaling = static_cast<float>(sum * sum);
  }

  void SetChannelMode(int chanTri) {
    mChanTri = std::clamp(chanTri, 0, 2);
  }

  void SetFFTSize(int fftSize) { SetFFTSizeAndOverlap(fftSize, mOverlap, mWindowType); }

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

// Freeze (冻结) 支持: UI 线程离线分析一帧原始样本 (冻结重算预热, 与实时路径共用实现)
  void PrepareFrameUI(Data &d) { PrepareDataForUI(d); }

  // Freeze (冻结) 支持: 输入侧 hop 相位查询 (音频线程 pending 计数, 冻结回放帧格对齐用)
  int HopPhase() const { return mBufCount; }

  // Freeze (冻结) 支持: 复位分析侧运行态 (窗历史), 不动输入侧 mBufCount/mPending
  // (输入计数冻结期间保持, 解冻后实时帧格无缝续接)
  void ResetRuntimeState() {
    for (auto &h : mHistory)
      h.fill(0.f);
  }

protected:
  void PrepareDataForUI(Data &d) override {
    const int nCh = std::min(d.nChans, MAXNC);
    const bool needL = (mChanTri != 2);
    const bool needR = (mChanTri != 2);
    const bool needSum = (mChanTri == 2);

    for (int c = d.chanOffset; c < d.chanOffset + nCh; ++c) {
      if ((c == 0 && !needL) || (c == 1 && !needR) || (c == 2 && !needSum)) {
        for (int i = 0; i < MAX_FFT_SIZE; ++i)
          d.vals[c][i] = 0.0f;
        continue;
      }

      std::memmove(mHistory[c].data(), mHistory[c].data() + mHop, (mFFTSize - mHop) * sizeof(float));
      std::memcpy(mHistory[c].data() + mFFTSize - mHop, d.vals[c].data(), mHop * sizeof(float));

      WDL_FFT_REAL *rb = mRealBuf[c].data();
      for (int i = 0; i < mFFTSize; ++i) {
        rb[i] = mHistory[c][i] * mWindow[i];
      }
      WDL_real_fft(rb, mFFTSize, 0);

      const WDL_FFT_COMPLEX *comp = reinterpret_cast<const WDL_FFT_COMPLEX *>(rb);
      const int halfSize = mFFTSize / 2;
      const float invScaling2 = 2.0f / mScaling;

      // DC (bin 0)
      const float dc = comp[0].re;
      d.vals[c][0] = std::sqrt((dc * dc) / mScaling);

      // 正频段 (bin 1 .. mNumBins - 1)
      for (int i = 1; i < mNumBins; ++i) {
        const int si = WDL_fft_permute(halfSize, i);
        const float re = comp[si].re, im = comp[si].im;
        d.vals[c][i] = std::sqrt((re * re + im * im) * invScaling2);
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
  int mChanTri = 0; // 0=LR, 1=PWR, 2=SUM
  int mWindowType = 0; // 0=Hann, 1=BH4
  float mScaling = 0.f;
  std::array<float, MAX_FFT_SIZE> mWindow{};
  std::array<std::array<float, MAX_FFT_SIZE>, MAXNC> mHistory{};
  std::array<std::array<WDL_FFT_REAL, MAX_FFT_SIZE>, MAXNC> mRealBuf{};
  std::array<std::array<float, MAX_FFT_SIZE>, MAXNC> mPending{};
};

END_IPLUG_NAMESPACE
