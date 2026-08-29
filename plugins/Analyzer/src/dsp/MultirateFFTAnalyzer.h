#pragma once

// MultirateFFTAnalyzer — 多速率八度子带 FFT 频谱分析引擎 (MR-FFT)
//
// 半波抽取链固定最小相位形态 (与 PAZ 共用 HalfbandDec2.h): 幅频逐点不变 (FFT 静态
// 读数/层边界折返不受影响), 深层链延迟 532→76ms @48kHz。

#ifndef STANDALONE_TEST
#include "ISender.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#include "HalfbandDec2.h"

BEGIN_IPLUG_NAMESPACE


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

  // Freeze (冻结) 支持: 复位分析侧运行态 (FFT 历史/层/抽取器), 不动输入侧
  // mBufCount/mPending 与 band 表 —— 冻结回放的确定性起点 (等价于引擎冷启动)
  void ResetRuntimeState() {
    for (int c = 0; c < MAXNC; ++c) {
      for (int l = 0; l < kMaxLayers - 1; ++l)
        mDecim[c][l].Reset();
      for (int l = 0; l < kMaxLayers; ++l) {
        mLayers[c][l].assign(kHop >> l, 0.f);
        mFftHist[c][l].assign(mFftSize[l], 0.f);
      }
    }
  }

  // Freeze (冻结) 支持: 输入侧 hop 相位查询 (音频线程 pending 计数, 冻结回放帧格对齐用)
  int HopPhase() const { return mBufCount; }

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

      // 提取频带能量: 带内 bin 幅度平方和开方, poolNorm 归一到中心单音幅度 A。
      for (int b = 0; b < nb; ++b) {
        const Band &bd = mBands[b];
        const float *mags = mMagBuf[c][bd.layer].data();
        float e = 0.f;
        for (int k = bd.binLo; k <= bd.binHi; ++k)
          e += mags[k] * mags[k];
        d.vals[c][b] = std::sqrt(e) * bd.poolNorm;
      }

      for (int b = nb; b < MAX_BANDS; ++b)
        d.vals[c][b] = 0.f;
    }
  }

private:
  struct Band {
    int layer;         // 所属金字塔层级 (0..9)
    int binLo;         // 带内能量池化的 bin 区间 [binLo, binHi]
    int binHi;
    float poolNorm;    // 中心单音增益归一 1/sqrt(Σ r_k²), r_k = |W(2π(k−kc)/N)|/W(0)
  };

  void InitLayerWindows() {
    constexpr double kPi = 3.14159265358979323846;
    for (int l = 0; l < kMaxLayers; ++l) {
      // 尺寸下限 256: bpo=24 时深层 (旧 64 点) 每 band 仅 ~0.5 bin, 相邻 band
      // 共享 kPeak 造成阶梯与锯齿; 下限 256 后每 band ≥2 个独立 bin
      const int sz = std::max(1024 >> l, 256);
      mFftSize[l] = sz;
      mWindows[l].resize(sz);
      double sum = 0.0;
      for (int i = 0; i < sz; ++i) {
        // 4 项 Blackman-Harris 窗: 旁瓣 -92 dB (旧 Hamming 仅 -42 dB, 强音裙边拖满整层)
        const double theta = 2.0 * kPi * i / (sz - 1);
        const double w = 0.35875 - 0.48829 * std::cos(theta)
                                 + 0.14128 * std::cos(2.0 * theta)
                                 - 0.01168 * std::cos(3.0 * theta);
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
    // 半波抽取链固定最小相位形态 (|G|=|A| 逐点不变, 分解因子全局共享只算一次)
    for (int c = 0; c < MAXNC; ++c)
      for (int l = 0; l < kMaxLayers - 1; ++l)
        mDecim[c][l].SetMinPhase(true);
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

      // 带内能量池化的 bin 区间: [fc−Δf/2, fc+Δf/2] 映射到本层 FFT 的 bin 下/上界。
      // 单 bin 采样的幅度服从瑞利分布 (对噪声类内容每个 band 是一次独立抽签, 逐层
      // 重复即呈"倍频状梳齿伪峰"); 池化后方差随 bin 数下降, 且带内能量对音调的
      // 落 bin 位置不敏感 (主瓣能量守恒, BH4 旁瓣 -92dB), 无需扇贝补偿。
      const double layerFs = fs / (double)(1 << layer);
      const int nFft = mFftSize[layer];
      const double sp = 0.5 * fc * (std::pow(2.0, 1.0 / mBpo) - 1.0);
      const int nBins = nFft / 2;
      int lo = (int)std::floor((fc - sp) * (double)nFft / layerFs);
      int hi = (int)std::ceil((fc + sp) * (double)nFft / layerFs);
      lo = std::clamp(lo, 1, nBins - 1);
      hi = std::clamp(hi, lo, nBins - 1);

      // 池化能量归一: r_k = |W(2π(k−kc)/N)|/W(0) 为中心单音在第 k bin 的幅度系数,
      // sqrt(Σ r_k²) 即池化路径对中心单音的增益, 取其倒数使中心单音读数恰为 A。
      // 必须取复数模 (窗对称中心在 (N−1)/2; 只取实部/丢窗索引会差数 dB 且随池宽变化)。
      double poolGain = 1.0;
      {
        constexpr double kPi = 3.14159265358979323846;
        const std::vector<float> &win = mWindows[layer];
        const double kFracC = fc * (double)nFft / layerFs;
        double w0 = 0.0;
        for (double w : win)
          w0 += w;
        double s2 = 0.0;
        for (int m = lo; m <= hi; ++m) {
          double rRe = 0.0, rIm = 0.0;
          for (int i = 0; i < (int)win.size(); ++i) {
            const double ang = 2.0 * kPi * (double)(m - kFracC) * (double)i / (double)nFft;
            rRe += (double)win[i] * std::cos(ang);
            rIm -= (double)win[i] * std::sin(ang);
          }
          const double r = std::sqrt(rRe * rRe + rIm * rIm) / w0;
          s2 += r * r;
        }
        if (s2 > 1e-12)
          poolGain = std::sqrt(s2);
      }

      Band bd;
      bd.layer = layer;
      bd.binLo = lo;
      bd.binHi = hi;
      bd.poolNorm = (float)(1.0 / poolGain);

      mBands.push_back(bd);
      mFreqs.push_back(fc);
    }

    ResetRuntimeState();
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
