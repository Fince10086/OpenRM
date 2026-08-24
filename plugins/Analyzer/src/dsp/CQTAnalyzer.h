#pragma once

// CQTAnalyzer — 恒定 Q 频谱分析器 (直接 CQT / 变长窗 DFT)。
//
// 实现对照: J.C. Brown 1991 原始 CQT 定义 + IEM CQT Analyzer (AES 2020) 的
// 变长窗思想。每个 band 用与自身频率分辨率匹配的窗长做单频 DFT:
//   - 低频 (<250Hz): 固定 Hz 分辨率 (40/20/10Hz), 窗长 = fs/lfRes
//   - 高频 (>=250Hz): 恒定 Q (Q=10), 窗长 = Q·fs/fc
// Hann 窗 + 预计算复数内核, 块式 hop 点积 (无逐样本递归, 无实时三角计算),
// 因此 CPU 远低于逐样本复解调滤波器组, 且 Hann 主瓣保证 band 间选择性
// (一阶低通 6dB/oct 滚降会把相邻 band 能量串进来, 导致曲线被抹平)。
// 每 band 独立时间窗 → 高频短窗快更新、低频长窗稳更新 (cqt 特性)。
// 幅度归一化: /(Σw/2) 恢复输入幅度, /sqrt(bw) 对齐 FFT power-per-bin 语义。
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

  static constexpr double kLowSplitHz = 250.0; // 低频固定分辨率与恒定 Q 的分界
  static constexpr double kHighQ = 10.0;       // 高频段恒定 Q
  static constexpr int kHop = 1024;            // 每 hop 样本发一帧幅度 (≈21ms @48k)

  CQTAnalyzer() { RebuildBands(); }

  void SetLfRes(int lfResHz) {
    const int lf = std::clamp(lfResHz, 10, 40);
    if (lf != mLfRes) {
      mLfRes = lf;
      RebuildBands();
    }
  }

  void SetSampleRate(double sr) {
    if (sr != mSampleRate) {
      mSampleRate = sr;
      RebuildBands();
    }
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
    float invSqrtBw;                  // 1/sqrt(bw): 功率密度归一化
    int winLen;                       // 窗长 (样本)
    std::vector<float> kernelRe, kernelIm; // w[n]·cos(2πfc n/fs) / w[n]·sin(...)
  };

  void RebuildBands() {
    mBands.clear();
    mFreqs.clear();
    const double fs = std::max(mSampleRate, 1.0);

    // 低频: 固定 Hz 分辨率, 从 20Hz 起按 lfRes 间隔直到 250Hz
    for (double fc = 20.0; fc < kLowSplitHz; fc += mLfRes)
      AddBand(fc, mLfRes, fs);
    // 高频: 恒定 Q, 相邻 band 3dB 相接 (频率比 r = (2Q+1)/(2Q-1))
    const double r = (2.0 * kHighQ + 1.0) / (2.0 * kHighQ - 1.0);
    for (double fc = kLowSplitHz; fc <= 20000.0; fc *= r)
      AddBand(fc, fc / kHighQ, fs);

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
    bd.winLen = std::max(8, (int)std::round((fc < kLowSplitHz) ? fs / mLfRes : kHighQ * fs / fc));
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

  int mLfRes = 40;
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
