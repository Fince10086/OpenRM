#pragma once

// VQTAnalyzer — 多速率变分辨率 Q 变换 (Multirate VQT) 频谱分析引擎

#include "ISender.h"

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

// 半带 ×2 抽取器 (跨帧保持滤波状态)。系数为 17 抽头 Hamming 窗半带 (截止 π/2, DC 增益 1),
// 偶数序 (除中心) 抽头严格为零 → 每输出样本 9 次乘加。流式输出保持全局偶数位抽取相位与尾部状态。要求 nin 为偶数。
struct HalfbandDec2 {
  static constexpr int kN = 17;                   // 抽头数 (奇数, 半带)
  static constexpr int kQ = (kN - 1) / 2;         // 中心抽头序号 8
  std::array<float, kN> mTap{};                   // 半带系数
  std::array<float, kN - 1> mState{};             // 最近 kN-1 个输入样本 (跨帧状态)
  float mWork[kN - 1 + 4096];                     // state ++ in 工作区 (先拷贝, 支持 in==out)

  HalfbandDec2() { BuildTaps(); }

  void BuildTaps() {
    // h[n] = w[n]·0.5·sinc((n-M)/2), Hamming 窗, 截止 π/2; 归一化 DC 增益 = 1。
    double taps[kN];
    double sum = 0.0;
    for (int i = 0; i < kN; ++i) {
      const int n = i - kQ;
      double v;
      if (n == 0)
        v = 0.5;
      else if ((n & 1) == 0)
        v = 0.0;                                  // 偶序 (除中心) = 0: 半带结构
      else
        v = 0.5 * std::sin(PI * n / 2.0) / (PI * n / 2.0);
      v *= 0.54 - 0.46 * std::cos(2.0 * PI * i / (kN - 1));
      taps[i] = v;
      sum += v;
    }
    for (int i = 0; i < kN; ++i)
      mTap[i] = (float)(taps[i] / sum);
  }

  void Reset() { mState.fill(0.f); }

  // nin 必须为偶数; 输出 nin/2 个样本到 out。out 可与 in 别名 (输入先拷贝到 mWork)。
  int Process(const float* in, int nin, float* out) {
    const int sz = (kN - 1) + nin;
    for (int i = 0; i < kN - 1; ++i)
      mWork[i] = mState[i];
    for (int i = 0; i < nin; ++i)
      mWork[(kN - 1) + i] = in[i];
    const int nout = nin / 2;
    for (int j = 0; j < nout; ++j) {
      const int p = 2 * j;                        // 全局偶数位抽取
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

// MAX_BANDS 须与 SpectrumSTFT 的 MAX_FFT_SIZE / SpectrumPad 的 TDataPacket 保持一致:
template <int MAXNC = 3, int QUEUE_SIZE = 64, int MAX_BANDS = 8192>
class VQTAnalyzer : public ISender<MAXNC, QUEUE_SIZE, std::array<float, MAX_BANDS>> {
public:
  using TDataPacket = std::array<float, MAX_BANDS>;
  using Data = ISenderData<MAXNC, TDataPacket>;
  using Base = ISender<MAXNC, QUEUE_SIZE, TDataPacket>;

  static constexpr double kFreqLo = 20.0;
  static constexpr double kFreqHi = 20000.0;
  static constexpr int kHop = 1024; // 每 hop 样本发一帧幅度 (≈21ms @48k)

  // 多速率金字塔参数
  static constexpr int kMaxLayers = 10;      // 层 0..9; 层 L 速率 = fs/2^L (最大 D = 512)
  static constexpr double kGuard = 0.78;     // band 上边距该层新奈奎斯特的比例 (防混叠)
  static constexpr double kCycleFloor = 4.0; // 深层窗长下限: 至少覆盖 ~4 个 fc 周期 (稳定)

  VQTAnalyzer() {
    for (int c = 0; c < MAXNC; ++c) {
      mPending[c].assign(kHop, 0.f);
      mLayers[c].resize(kMaxLayers);
    }
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
    const int g = std::clamp(gammaHz, 5, 20);
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

  void SetChannelMode(int chanTri) {
    mChanTri = std::clamp(chanTri, 0, 2);
  }

  // UI 线程: 若音频线程请求了重建 (γ/BPO/采样率变化), 惰性重建 band 表与金字塔。
  void CheckRebuild() {
    if (mNeedRebuild.exchange(false))
      RebuildBands();
  }

  int NumBands() const { return (int)mBands.size(); }
  const std::vector<double> &BandFreqs() const { return mFreqs; }

  // 音频线程: 仅采集样本, 每 kHop 个样本入队一帧原始 hop 数据 (不做任何计算)。
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

protected:
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

      const float *raw = d.vals[c].data();

      // 金字塔降采样
      AppendLayer(c, 0, raw, kHop, mMaxWinPerLayer[0]);
      const float *cur = raw;
      int nin = kHop;
      for (int l = 0; l < kMaxLayers - 1; ++l) {
        float *dst = mScratch[(l & 1)].data();
        const int nout = mDecim[c][l].Process(cur, nin, dst);
        if (nout > 0)
          AppendLayer(c, l + 1, dst, nout, mMaxWinPerLayer[l + 1]);
        cur = dst;
        nin = nout;
      }

      // band 点积
      for (int b = 0; b < nb; ++b) {
        const Band &bd = mBands[b];
        RunState &rs = mRun[c][b];
        float mag;
        if (--rs.phase <= 0) {
          const std::vector<float> &buf = mLayers[c][bd.layer];
          const int wl = bd.winLen;
          // 跨层延迟对齐
          const int have = std::max(0, std::min(wl, (int)buf.size() - bd.readOff));
          const int skip = wl - have;
          const int start = std::max(0, (int)buf.size() - bd.readOff - have);
          const float *x = buf.data() + start;
          const float *kr = bd.kernelRe.data() + skip;
          const float *ki = bd.kernelIm.data() + skip;
          const int cnt = wl - skip;
          float re0 = 0.f, re1 = 0.f, re2 = 0.f, re3 = 0.f;
          float im0 = 0.f, im1 = 0.f, im2 = 0.f, im3 = 0.f;
          int j = 0;
          for (; j + 4 <= cnt; j += 4) {
            const float v0 = x[j], v1 = x[j + 1], v2 = x[j + 2], v3 = x[j + 3];
            re0 += v0 * kr[j];     im0 += v0 * ki[j];
            re1 += v1 * kr[j + 1]; im1 += v1 * ki[j + 1];
            re2 += v2 * kr[j + 2]; im2 += v2 * ki[j + 2];
            re3 += v3 * kr[j + 3]; im3 += v3 * ki[j + 3];
          }
          float re = (re0 + re1) + (re2 + re3);
          float im = (im0 + im1) + (im2 + im3);
          for (; j < cnt; ++j) {
            re += x[j] * kr[j];
            im += x[j] * ki[j];
          }
          mag = std::sqrt(re * re + im * im) * bd.wsumInv;
          rs.last = mag;
          rs.phase = bd.advance;
        } else {
          mag = rs.last;
        }
        d.vals[c][b] = mag;
      }
      for (int b = nb; b < MAX_BANDS; ++b)
        d.vals[c][b] = 0.0f;
    }
  }

private:
  struct Band {
    int layer;
    float wsumInv;
    int winLen;
    int advance;
    int readOff;
    std::vector<float> kernelRe, kernelIm; // 在该层速率下计算的 w·cos/w·sin
  };

  // 逐 band 运行状态
  struct RunState {
    int phase = 0;
    float last = 0.f;
  };

  void AppendLayer(int c, int l, const float *src, int n, int maxKeep) {
    std::vector<float> &buf = mLayers[c][l];
    if (n >= maxKeep) {
      buf.assign(src + (n - maxKeep), src + n);
      return;
    }
    const int oldKeep = maxKeep - n;
    if ((int)buf.size() > oldKeep)
      buf.erase(buf.begin(), buf.begin() + (buf.size() - oldKeep));
    buf.insert(buf.end(), src, src + n);
  }

  void RebuildBands() {
    mBands.clear();
    mFreqs.clear();
    mMaxWinPerLayer.fill(0);
    const double fs = std::max(mSampleRate, 1.0);
    const double q = 1.0 / (std::pow(2.0, 1.0 / mBpo) - 1.0);

    // 列出全部 band 的频率/带宽/层号
    struct Spec { double fc, bw; int layer; };
    std::vector<Spec> spec;
    std::array<bool, kMaxLayers> used{};
    for (int k = 0;; ++k) {
      const double fc = kFreqLo * std::pow(2.0, (double)k / mBpo);
      if (fc > kFreqHi)
        break;
      const double bw = fc / q + mGamma;
      const int L = AssignLayer(fc + bw / 2.0, fs);
      spec.push_back({fc, bw, L});
      used[L] = true;
    }

    // 逐层构建 band 并跨层延迟对齐
    for (int i = 0, n = (int)spec.size(); i < n;) {
      const int L = spec[i].layer;
      int j = i;
      while (j < n && spec[j].layer == L)
        ++j; // 本层 band 段 [i, j)
      const double gdL = GroupDelaySec(fs, L);
      const double gdD = (L + 1 < kMaxLayers && used[L + 1]) ? GroupDelaySec(fs, L + 1) : gdL;
      for (int t = i; t < j; ++t) {
        const double f = (j - i > 1) ? (double)(t - i) / (j - i - 1) : 0.0;
        const double R = gdD + (gdL - gdD) * f;
        AddBand(spec[t].fc, spec[t].bw, fs, L, R);
      }
      i = j;
    }

    for (int c = 0; c < MAXNC; ++c)
      mRun[c].assign((size_t)mBands.size(), RunState{0, 0.f});

    for (int c = 0; c < MAXNC; ++c) {
      for (int l = 0; l < kMaxLayers; ++l)
        mLayers[c][l].clear();
    }
    for (int c = 0; c < MAXNC; ++c)
      for (int l = 0; l < kMaxLayers; ++l)
        mDecim[c][l].Reset();
  }

  int AssignLayer(double bandHi, double fs) const {
    int L = 0;
    while (L + 1 < kMaxLayers) {
      const double newNyq = fs / (double)(1 << (L + 2));
      if (bandHi > kGuard * newNyq)
        break;
      ++L;
    }
    return L;
  }

  static double GroupDelaySec(double fs, int L) {
    return (double)detail::HalfbandDec2::kQ / fs * (std::pow(2.0, L) - 1.0);
  }

  void AddBand(double fc, double bw, double fs, int L, double R) {
    const double fsL = fs / (double)(1 << L);

    const int wl = std::max(8, std::max((int)std::round(fsL / bw),
                                        (int)std::round(fsL / fc * kCycleFloor)));

    Band bd;
    bd.layer = L;
    bd.winLen = wl;
    bd.wsumInv = (float)(4.0 / wl);
    bd.advance = 1;
    bd.readOff = (int)std::lround((R - GroupDelaySec(fs, L)) * fsL);
    bd.kernelRe.resize(wl);
    bd.kernelIm.resize(wl);
    const double step = 2.0 * PI * fc / fsL; // 该层速率的归一化频率
    for (int n = 0; n < wl; ++n) {
      const double w = 0.5 * (1.0 - std::cos(2.0 * PI * n / (wl - 1)));
      const double ph = step * n;
      bd.kernelRe[n] = (float)(w * std::cos(ph));
      bd.kernelIm[n] = (float)(w * std::sin(ph));
    }
    mBands.push_back(std::move(bd));
    mFreqs.push_back(fc);
    mMaxWinPerLayer[L] = std::max(mMaxWinPerLayer[L], wl + bd.readOff);
  }

  int mBpo = 24;                          // bins per octave (12/24)
  int mGamma = 20;                        // 低频带宽下限 Hz
  int mChanTri = 0;                       // 0=LR, 1=PWR, 2=SUM
  double mSampleRate = 48000.0;
  std::atomic<bool> mNeedRebuild{false};
  std::vector<Band> mBands;
  std::vector<double> mFreqs;
  std::array<int, kMaxLayers> mMaxWinPerLayer{};
  std::array<std::vector<std::vector<float>>, MAXNC> mLayers;
  std::array<std::array<detail::HalfbandDec2, kMaxLayers>, MAXNC> mDecim;
  std::array<std::vector<RunState>, MAXNC> mRun;
  std::array<float, kHop> mScratch[2];
  int mBufCount = 0;
  std::array<std::vector<float>, MAXNC> mPending;
};

END_IPLUG_NAMESPACE
