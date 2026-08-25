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
// 线程模型: 音频线程仅在 ProcessBlock 内采集每通道 kHop 个原始样本并入队
// (与 SpectrumSTFT 一致, 不做任何分析); band 内核重建与全部点积计算都发生在
// UI 线程 PrepareDataForUI 中 (由 OnIdle → TransmitData 调用)。γ/BPO/采样率
// 变化时音频线程只写标量配置并置位 mNeedRebuild, band 表与历史缓冲的惰性重建
// 在下一个 UI 线程时机执行 —— mBands/mHistory 由 UI 线程单线独占, 消除了跨线程
// vector 重分配竞争, 也不再把重计算压在实时线程上。
//
// 数据经 ISender 发送: TDataPacket 前 kHop 个元素为入队的原始 hop 样本,
// PrepareDataForUI 将其替换为各通道 band 幅度 (前 nBands 个元素)。

#include "ISender.h"

#include <algorithm>
#include <array>
#include <atomic>
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

  CQTAnalyzer() {
    for (int c = 0; c < MAXNC; ++c)
      mPending[c].assign(kHop, 0.f);
    RebuildBands();
  }

  // 返回是否实际请求了重建 (参数与当前值相同则返回 false)。调用方据此决定
  // 是否需要通知显示层重置。实际重建发生在 UI 线程 (CheckRebuild/PrepareDataForUI)。
  bool SetBpo(int bpo) {
    const int b = (bpo == 12) ? 12 : 24;
    if (b != mBpo) {
      mBpo = b;
      mNeedRebuild.store(true);
      return true;
    }
    return false;
  }

  bool SetGamma(int gammaHz) {
    const int g = std::clamp(gammaHz, 10, 40);
    if (g != mGamma) {
      mGamma = g;
      mNeedRebuild.store(true);
      return true;
    }
    return false;
  }

  bool SetSampleRate(double sr) {
    if (sr != mSampleRate) {
      mSampleRate = sr;
      mNeedRebuild.store(true);
      return true;
    }
    return false;
  }

  // UI 线程: 若音频线程请求了重建 (γ/BPO/采样率变化), 惰性重建 band 表与历史缓冲。
  // 发送 band 频率表/幅度数据前必须先调用, 保证读取到的 bands/freqs 与最新配置一致。
  void CheckRebuild() {
    if (mNeedRebuild.exchange(false))
      RebuildBands();
  }

  int NumBands() const { return (int)mBands.size(); }
  const std::vector<double> &BandFreqs() const { return mFreqs; }

  // 音频线程: 仅采集样本, 每 kHop 个样本入队一帧原始 hop 数据 (不做任何频谱计算)。
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

protected:
  // UI 线程: 弹出的数据包含各通道最新 kHop 个原始样本, 先追加进该通道历史缓冲,
  // 再计算全部 band 幅度写回 d.vals (TransmitData 随后将其发送给显示控件)。
  // 与 SpectrumSTFT 一致, 频谱计算整体从实时线程剥离; 各通道历史由 UI 线程独占。
  void PrepareDataForUI(Data &d) override {
    CheckRebuild();
    const int nb = NumBands();
    if (nb <= 0)
      return;
    const int nCh = std::min(d.nChans, MAXNC);

    for (int c = 0; c < nCh; ++c) {
      // 追加 hop 个新样本到历史缓冲 (仅保留最近 mMaxWinLen 个)
      float *hist = mHistory[c].data();
      if (mHistLen + kHop <= mMaxWinLen) {
        std::memcpy(hist + mHistLen, d.vals[c].data(), kHop * sizeof(float));
        mHistLen += kHop;
      } else {
        const int keep = mMaxWinLen - kHop;
        if (keep > 0)
          std::memmove(hist, hist + mHistLen - keep, (size_t)keep * sizeof(float));
        std::memcpy(hist + keep, d.vals[c].data(), kHop * sizeof(float));
        mHistLen = mMaxWinLen;
      }

      for (int b = 0; b < nb; ++b) {
        const Band &bd = mBands[b];
        const int wl = bd.winLen;
        const int have = std::min(wl, mHistLen); // 实际可用样本数
        const int skip = wl - have;              // 窗口前补零个数 (启动阶段)
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
  }

private:
  struct Band {
    float wsumInv;                    // 4/winLen: 恢复输入幅度
    float invSqrtBw;                  // 1/sqrt(Bk): 功率密度归一化
    int winLen;                       // 窗长 (样本) = fs/Bk
    std::vector<float> kernelRe, kernelIm; // w[n]·cos(2πfc n/fs) / w[n]·sin(...)
  };

  // 仅 UI 线程调用 (构造 / CheckRebuild)。不触碰音频线程独占的 mPending/mBufCount。
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
    for (int c = 0; c < MAXNC; ++c)
      mHistory[c].assign((size_t)mMaxWinLen + kHop, 0.f);
    mHistLen = 0;
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

  int mBpo = 24;      // bins per octave (12/24); 音频线程写, UI 线程读 (重建由 mNeedRebuild 触发)
  int mGamma = 40;    // 低频带宽下限 Hz (10/20/40)
  double mSampleRate = 48000.0;
  std::atomic<bool> mNeedRebuild{false}; // 音频线程置位, UI 线程读取并清除
  std::vector<Band> mBands;              // UI 线程独占
  std::vector<double> mFreqs;            // UI 线程独占
  int mMaxWinLen = 0;                    // UI 线程独占
  int mHistLen = 0;                      // UI 线程独占
  int mBufCount = 0;                     // 音频线程独占
  std::array<std::vector<float>, MAXNC> mHistory; // UI 线程独占
  std::array<std::vector<float>, MAXNC> mPending; // 音频线程独占 (构造时分配, 不再重分配)
};

END_IPLUG_NAMESPACE
