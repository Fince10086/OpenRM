#pragma once

// PAZAnalyzer — 心理声学临界频带频谱分析引擎 (多速率解调核算法)
//
// 每个频带一个 4 项 Blackman-Harris 窗复数解调 FIR 核: kernel[n] = w[n]·e^{-j2π·fc·n/fsL}。
// 核长取 T = 6/bw (bw 为频带表间距带宽) → 主瓣零点距 4/T = 0.67·bw, 邻带中心落在
// 1.5 倍零点距的第一旁瓣区 (峰值 −92dB), 正弦输入的邻带读数 ≤ −92dB;
// 带内偏移的 scalloping 也很浅 (≈半带宽偏移仅 −1dB), 与原版 PAZ 的读数形态吻合。
// (核长再短邻带隔离开始变差, 再长带间读数出现深谷 —— 6/bw 是两头的平衡点。)
// 旧实现 (2×2 阶 TPT SVF 级联带通 + hop 内峰值) 受 4 阶滚降限制, 9.5% 邻距下邻带
// 读数 −12~−32dB, 正弦输入必然点亮数个邻带; 核算法达成与原版 PAZ 一致的隔离形态。
//
// 多速率: 核跑在 2x 半带抽取金字塔上 (层 L 速率 fs/2^L, 跨帧保持滤波状态), 层分配
// 与 VQT 同款 (band 顶 ≤ 0.78·新奈奎斯特); 跨层 readOff 对齐消除层边界群延迟错位。
// 低频核长可达 ~0.7s (6Hz 带), 由深层抽取摊薄计算量; 冷启动/重建后低频带约需
// 核长时间收敛到稳态 (与 VQT 同性质)。

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
// 阻带跌落 >90 dB, 杜绝层边界强单音穿透抽取器折返到下层的"假频谱峰"。偶数序 (除中心) 抽头严格为零。
// 线性相位, 群延迟 = kQ 输入样本。要求 nin 为偶数。
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
      const double theta = 2.0 * kPi * i / (kN - 1); // 4 项 Blackman-Harris: 旁瓣 -92 dB
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

template <int MAXNC = 3, int QUEUE_SIZE = 64, int MAX_BANDS = 8192>
class PAZAnalyzer : public ISender<MAXNC, QUEUE_SIZE, std::array<float, MAX_BANDS>> {
public:
  using TDataPacket = std::array<float, MAX_BANDS>;
  using Data = ISenderData<MAXNC, TDataPacket>;
  using Base = ISender<MAXNC, QUEUE_SIZE, TDataPacket>;

  static constexpr int kHop = 1024;
  static constexpr int kMaxLayers = 10;
  static constexpr double kGuard = 0.78; // band 上边距该层新奈奎斯特的比例 (防抽取混叠, VQT 同款)

  PAZAnalyzer() {
    for (int c = 0; c < MAXNC; ++c)
      mPending[c].assign(kHop, 0.f);
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

  // 核长系数 k (T = k/bw, BH4 零点距 4/T = 4bw/k)。k 越小读数越平/延迟越低,
  // 但邻带 (间距 0.87~1.2×bw) 逐渐滑入主瓣。实测 (48k, 40/10Hz 模式边沿探针):
  //   k=8/7/6/5.5/5.0 → 最坏邻带 −94/−93/−93/−92/−92 dB (旁瓣地板);
  //   k=4.5/4.0/3.0 → −65/−45/−20 dB (最坏邻带过不了第一零点, 悬崖在 k≈4.6)。
  // 带间谷深 (音落两带正中): k=6 → −37dB, k=5 → −24dB。改动触发重建 (冷启动重新收敛)。
  bool SetKernelLen(double k) {
    const double v = std::clamp(k, 2.0, 10.0);
    if (std::abs(v - mKernelLen) > 1e-6) {
      mKernelLen = v;
      mNeedRebuild.store(true, std::memory_order_release);
      return true;
    }
    return false;
  }
  double KernelLen() const { return mKernelLen; }

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

// Freeze (冻结) 支持: UI 线程离线分析一帧原始样本 (冻结重算预热, 与实时路径共用实现)
  void PrepareFrameUI(Data &d) { PrepareDataForUI(d); }

  // Freeze (冻结) 支持: 复位分析侧运行态 (层缓冲/抽取器), 不动输入侧
  // mBufCount/mPending 与 band 表 —— 冻结回放的确定性起点 (等价于引擎冷启动)
  void ResetRuntimeState() {
    for (int c = 0; c < MAXNC; ++c) {
      for (int l = 0; l < kMaxLayers; ++l)
        mLayers[c][l].clear();
      for (int l = 0; l < kMaxLayers - 1; ++l)
        mDecim[c][l].Reset();
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
      AppendLayer(c, 0, d.vals[c].data(), kHop);
      const float *cur = d.vals[c].data();
      int nin = kHop;
      for (int l = 0; l < kMaxLayers - 1; ++l) {
        float *dst = mScratch[(l & 1)].data();
        nin = mDecim[c][l].Process(cur, nin, dst);
        AppendLayer(c, l + 1, dst, nin);
        cur = dst;
      }

      // band 点积 (复数解调核相关, 幅度 = |Σ x·w·e^{jωn}|·2/Σw)
      for (int b = 0; b < nb; ++b) {
        const Band &bd = mBands[b];
        const std::vector<float> &buf = mLayers[c][bd.layer];
        const int wl = bd.winLen;
        // readOff 夹非负 + 冷启动短缓冲部分窗 (零窗长跳过): 取窗不越界
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
        d.vals[c][b] = std::sqrt(re * re + im * im) * bd.wsumInv;
      }

      for (int b = nb; b < MAX_BANDS; ++b)
        d.vals[c][b] = 0.f;
    }
  }

private:
  struct Band {
    int layer;                            // 速率层 (层 L 速率 = fs/2^L)
    int winLen;                           // 本层速率核长
    int readOff;                          // 跨层延迟对齐: 取窗终点距缓冲末端的本层样本数
    float wsumInv;                        // 2/Σw (带中心正弦幅度归一)
    std::vector<float> kernelRe, kernelIm; // 本层速率的 w·cos/w·sin
  };

  static constexpr double kPi = 3.14159265358979323846;

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

  // 频带依频率升序 → 层号非增, 同层带构成连续段
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

  // 半带链总群延迟 (输入样本): 每级线性相位 kQ 输入样本, 到层 L 共 Σ kQ·2^k
  static double ChainGd(int L) {
    return (double)detail::HalfbandDec2::kQ * (double)((1 << L) - 1);
  }

  void RebuildBands() {
    mBands.clear();
    mFreqs.clear();
    mMaxWinPerLayer.fill(0);
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

    struct Spec {
      double fc, bw;
      int layer;
    };
    std::vector<Spec> spec;
    for (int i = 0; i < numSrc; ++i) {
      const double fc = srcFreqs[i] * scale;
      if (fc >= nyqLimit)
        continue;

      // 带宽取频带表相邻间距 (首末带单边), 与显示网格一致
      double bw;
      if (i == 0)
        bw = (srcFreqs[1] - srcFreqs[0]) * scale;
      else if (i == numSrc - 1)
        bw = (srcFreqs[numSrc - 1] - srcFreqs[numSrc - 2]) * scale;
      else
        bw = 0.5 * (srcFreqs[i + 1] - srcFreqs[i - 1]) * scale;

      Spec sp{fc, bw, 0};
      sp.layer = AssignLayer(fc + bw / 2.0, fs);
      spec.push_back(sp);
    }

    std::array<int, kMaxLayers> layerCount{};
    for (const Spec &sp : spec)
      ++layerCount[sp.layer];

    for (int i = 0, n = (int)spec.size(); i < n;) {
      const int L = spec[i].layer;
      int j = i;
      while (j < n && spec[j].layer == L)
        ++j; // 本层 band 段 [i, j)
      // 下一更深带流层 (段顶 band 对齐到该层基准时刻)
      int nextDeep = -1;
      for (int d = L + 1; d < kMaxLayers; ++d)
        if (layerCount[d] > 0) {
          nextDeep = d;
          break;
        }
      for (int t = i; t < j; ++t) {
        const double fSeg = (j - i > 1) ? (double)(t - i) / (j - i - 1) : 0.0;
        AddBand(spec[t].fc, spec[t].bw, fs, L, nextDeep, fSeg);
      }
      i = j;
    }

    ResetRuntimeState();
  }

  void AddBand(double fc, double bw, double fs, int L, int nextDeep, double fSeg) {
    const double fsL = fs / (double)(1 << L);
    // 核长 T = k/bw (k 默认 5): 主瓣零点距 4/T = 0.8bw, 邻带中心 1.1~1.5 倍零点距
    // (旁瓣 −92dB); k 可由 SetKernelLen 调节以在隔离/读数平坦度/延迟间取平衡
    const int wl = std::max(8, (int)std::lround(mKernelLen * fsL / bw));
    Band bd;
    bd.layer = L;
    bd.winLen = wl;
    // 跨层延迟对齐: 段顶 band (fSeg=0) 对齐到下一深层基准时刻, 段底 (fSeg=1) 对齐到本层
    const double gdL = ChainGd(L);
    const double gdD = (nextDeep > 0) ? ChainGd(nextDeep) : gdL;
    const double R = gdD + (gdL - gdD) * fSeg;
    // 夹非负: 负 readOff 会让取窗起点越过缓冲区末端 (读越界)
    bd.readOff = std::max(0, (int)std::lround((R - gdL) / (double)(1 << L)));
    bd.kernelRe.resize(wl);
    bd.kernelIm.resize(wl);
    const double step = 2.0 * kPi * fc / fsL; // 该层速率的归一化频率
    double sum = 0.0;
    for (int n = 0; n < wl; ++n) {
      const double theta = 2.0 * kPi * n / (wl - 1);
      const double w = 0.35875 - 0.48829 * std::cos(theta)
                               + 0.14128 * std::cos(2.0 * theta)
                               - 0.01168 * std::cos(3.0 * theta);
      sum += w;
      const double ph = step * n;
      bd.kernelRe[n] = (float)(w * std::cos(ph));
      bd.kernelIm[n] = (float)(w * std::sin(ph));
    }
    bd.wsumInv = (sum > 1e-12) ? (float)(2.0 / sum) : 0.0f;
    mBands.push_back(std::move(bd));
    mFreqs.push_back(fc);
    mMaxWinPerLayer[L] = std::max(mMaxWinPerLayer[L], wl + bd.readOff);
  }

  // 层缓冲追加 (保留窗口覆盖所需尾部)
  void AppendLayer(int c, int l, const float *src, int n) {
    std::vector<float> &buf = mLayers[c][l];
    const int maxKeep = mMaxWinPerLayer[l];
    if (n >= maxKeep) {
      buf.assign(src + (n - maxKeep), src + n);
      return;
    }
    const int oldKeep = maxKeep - n;
    if ((int)buf.size() > oldKeep)
      buf.erase(buf.begin(), buf.begin() + (buf.size() - oldKeep));
    buf.insert(buf.end(), src, src + n);
  }

  double mSampleRate = 48000.0;
  double mKernelLen = 5.0; // 核长系数 k (T = k/bw)
  int mLfMode = 0; // 0=40Hz, 1=20Hz, 2=10Hz
  int mChanTri = 0; // 0=LR, 1=PWR, 2=SUM
  std::atomic<bool> mNeedRebuild{false};

  std::vector<Band> mBands;
  std::vector<double> mFreqs;
  std::array<int, kMaxLayers> mMaxWinPerLayer{}; // 该层需保留的本层样本数 (核长 + readOff)

  std::array<std::array<detail::HalfbandDec2, kMaxLayers - 1>, MAXNC> mDecim;
  std::array<std::array<std::vector<float>, kMaxLayers>, MAXNC> mLayers;
  std::array<std::array<float, kHop / 2>, 2> mScratch; // 金字塔级间缓冲 (乒乓)

  std::array<std::vector<float>, MAXNC> mPending;
  int mBufCount = 0;
};

END_IPLUG_NAMESPACE
