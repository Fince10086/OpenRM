#pragma once

// CQTAnalyzer — 变分辨率 CQT (VQT) 频谱分析器 (直接 CQT / 变长窗 DFT)。
//
// 实现对照: IEM CQT Analyzer (AES 2020) 的带宽公式 + J.C. Brown 1991 直接 CQT:
//   fk = f0·2^(k/B)          (B = bins per octave, 12/24)
//   Bk = fk/Q + γ            (γ = 低频带宽下限 Hz, 10/20/40)
//   Q  = 1/(2^(1/B) - 1)
//   Qnew = fk/Bk,  Nk = Qnew·fs/fk   (即 Nk = fs/Bk)
// 效果: 高频段保持恒定 Q, 低频段平滑过渡到恒定 Hz 带宽 (无 250Hz 硬切换 kink),
// 窗长随频率平滑变化, 低频窗长被钳在 ~1/γ (如 γ=20Hz → 50ms)。
// Hann 窗 + 预计算复数内核, 块式 hop 点积 (无逐样本递归, 无实时三角计算),
// 每 band 独立时间窗 → 高频短窗快更新、低频长窗稳更新 (cqt 特性)。
// 幅度归一化: /(Σw/2) 恢复输入幅度, /sqrt(Bk) 对齐 FFT power-per-bin 语义。
//
// 数据经 ISender 发送: TDataPacket 前 nBands 个元素为各通道 band 幅度。

#include "ISender.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

BEGIN_IPLUG_NAMESPACE

template <int MAXNC = 2, int QUEUE_SIZE = 64, int MAX_BANDS = 4096>
class CQTAnalyzer : public ISender<MAXNC, QUEUE_SIZE, std::array<float, MAX_BANDS>> {
public:
  using TDataPacket = std::array<float, MAX_BANDS>;
  using Data = ISenderData<MAXNC, TDataPacket>;
  using Base = ISender<MAXNC, QUEUE_SIZE, TDataPacket>;

  static constexpr double kFreqLo = 20.0;
  static constexpr double kFreqHi = 20000.0;
  static constexpr int kHop = 1024; // 每 hop 样本发一帧幅度 (≈21ms @48k)

  CQTAnalyzer() { RebuildBands(); }

  // 返回是否实际重建了 band 表 (参数与当前值相同则返回 false, 不重建)。
  // 调用方可据此决定是否需要通知显示层重置。
  bool SetBpo(int bpo) {
    const int b = (bpo == 12) ? 12 : 24;
    if (b != mBpo) {
      mBpo = b;
      RebuildBands();
      return true;
    }
    return false;
  }

  bool SetGamma(int gammaHz) {
    const int g = std::clamp(gammaHz, 10, 40);
    if (g != mGamma) {
      mGamma = g;
      RebuildBands();
      return true;
    }
    return false;
  }

  bool SetSampleRate(double sr) {
    if (sr != mSampleRate) {
      mSampleRate = sr;
      RebuildBands();
      return true;
    }
    return false;
  }

  int NumBands() const { return (int)mBands.size(); }
  const std::vector<double> &BandFreqs() const { return mFreqs; }

  void ProcessBlock(sample **inputs, int nFrames, int ctrlTag = kNoTag, int nChans = MAXNC,
                    int chanOffset = 0) {
    const int nCh = std::min(nChans, MAXNC);
    if (NumBands() <= 0)
      return;

    for (int s = 0; s < nFrames; ++s) {
      for (int c = 0; c < nCh; ++c)
        mPending[c][mBufCount] = (float)inputs[chanOffset + c][s];
      if (++mBufCount == kHop) {
        ComputeFrame(ctrlTag, nCh, chanOffset);
        mBufCount = 0;
      }
    }
  }

private:
  struct Band {
    float wsumInv;                    // 4/winLen: 恢复输入幅度
    float invSqrtBw;                  // 1/sqrt(Bk): 功率密度归一化
    int winLen;                       // 窗长 (样本) = fs/Bk
    std::vector<float> kernelRe, kernelIm; // w[n]·cos(2πfc n/fs) / w[n]·sin(...)
  };

  void RebuildBands() {
    mBands.clear();
    mFreqs.clear();
    const double fs = std::max(mSampleRate, 1.0);
    const double q = 1.0 / (std::pow(2.0, 1.0 / mBpo) - 1.0);

    for (int k = 0;; ++k) {
      const double fc = kFreqLo * std::pow(2.0, (double)k / mBpo);
      if (fc > kFreqHi)
        break;
      const double bw = fc / q + mGamma; // Bk: 低频段被 γ 托底, 高频段趋于恒定 Q
      AddBand(fc, bw, fs);
    }

    mMaxWinLen = 0;
    for (const Band &b : mBands)
      mMaxWinLen = std::max(mMaxWinLen, b.winLen);
    for (int c = 0; c < MAXNC; ++c) {
      mHistory[c].assign((size_t)mMaxWinLen + kHop, 0.f);
      mPending[c].assign(kHop, 0.f);
    }
    mHistLen = 0;
    mBufCount = 0;
  }

  void AddBand(double fc, double bw, double fs) {
    Band bd;
    bd.winLen = std::max(8, (int)std::round(fs / bw));
    bd.wsumInv = (float)(4.0 / bd.winLen);
    bd.invSqrtBw = (float)(1.0 / std::sqrt(bw));
    bd.kernelRe.resize(bd.winLen);
    bd.kernelIm.resize(bd.winLen);
    const double step = 2.0 * PI * fc / fs;
    for (int n = 0; n < bd.winLen; ++n) {
      const double w = 0.5 * (1.0 - std::cos(2.0 * PI * n / (bd.winLen - 1)));
      const double ph = step * n;
      bd.kernelRe[n] = (float)(w * std::cos(ph));
      bd.kernelIm[n] = (float)(w * std::sin(ph));
    }
    mBands.push_back(std::move(bd));
    mFreqs.push_back(fc);
  }

  void ComputeFrame(int ctrlTag, int nCh, int chanOffset) {
    const int nb = NumBands();
    Data d{ctrlTag, nCh, chanOffset};

    for (int c = 0; c < nCh; ++c) {
      // 追加 hop 个新样本到历史缓冲 (仅保留最近 mMaxWinLen 个)
      float *hist = mHistory[c].data();
      if (mHistLen + kHop <= mMaxWinLen) {
        std::memcpy(hist + mHistLen, mPending[c].data(), kHop * sizeof(float));
        mHistLen += kHop;
      } else {
        const int keep = mMaxWinLen - kHop;
        if (keep > 0)
          std::memmove(hist, hist + mHistLen - keep, (size_t)keep * sizeof(float));
        std::memcpy(hist + keep, mPending[c].data(), kHop * sizeof(float));
        mHistLen = mMaxWinLen;
      }

      for (int b = 0; b < nb; ++b) {
        const Band &bd = mBands[b];
        const int wl = bd.winLen;
        const int have = std::min(wl, mHistLen);  // 实际可用样本数
        const int skip = wl - have;               // 窗口前补零个数 (启动阶段)
        const float *x = hist + (mHistLen - have); // 最近 have 个样本
        double re = 0.0, im = 0.0;
        for (int n = skip; n < wl; ++n) {
          const float v = x[n - skip];
          re += v * bd.kernelRe[n];
          im += v * bd.kernelIm[n];
        }
        d.vals[c][b] = (float)(std::sqrt(re * re + im * im) * bd.wsumInv * bd.invSqrtBw);
      }
      for (int b = nb; b < MAX_BANDS; ++b)
        d.vals[c][b] = 0.0f;
    }
    Base::PushData(d);
  }

  int mBpo = 24;      // bins per octave (12/24)
  int mGamma = 40;    // 低频带宽下限 Hz (10/20/40)
  double mSampleRate = 48000.0;
  std::vector<Band> mBands;
  std::vector<double> mFreqs;
  int mMaxWinLen = 0;
  int mHistLen = 0;
  int mBufCount = 0;
  std::array<std::vector<float>, MAXNC> mHistory;
  std::array<std::vector<float>, MAXNC> mPending;
};

END_IPLUG_NAMESPACE
