#pragma once

// PAZAnalyzer — 心理声学临界频带频谱分析引擎 (多速率解调核算法)
//
// 每个频带一个复数解调 FIR 核: kernel[n] = proto[n]·e^{-j2π·fc·n/fsL}。
// 核形状: Kaiser 等波纹原型低通, 核长系数 K=4.6
//
// 多速率: 核跑在 2x 半带抽取金字塔上 (层 L 速率 fs/2^L, 跨帧保持滤波状态)
//
// 最小相位化: 带核原型与半波抽取链都做同幅频谱分解 (倒频谱法)

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
  // 离线验收钩子: 暴露谱分解与贝塞尔函数供数值探针比对
  static std::vector<double> TestMinPhaseFactor(const std::vector<double> &proto) {
    return detail::MinPhaseFactor(proto);
  }
  static double TestBesselI0(double x) { return BesselI0(x); }
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

  // 半带链总群延迟 (输入样本): 每级 GdPerStage() (最小相位能量重心 ≈7.2), 到层 L 共 Σ gd·2^k
  double ChainGd(int L) const {
    return detail::HalfbandDec2::GdPerStage() * (double)((1 << L) - 1);
  }

  void RebuildBands() {
    // 半波抽取链固定最小相位形态 (分解因子全局共享只算一次)
    for (int c = 0; c < MAXNC; ++c)
      for (int l = 0; l < kMaxLayers - 1; ++l)
        mDecim[c][l].SetMinPhase(true);
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
    // Kaiser 等波纹原型低通 —— 平顶到 0.37bw, 阻带墙 Esb=0.13·K·bw 起 −68dB
    // (K=4.6 → Esb≈0.60bw, 原版拟合定值); 过渡带越窄核越长: T = (A−8)/(2.285·2π·(Esb−Epb))。
    // 最小相位谱分解要求奇长实对称原型, 偶长 +1。
    int wl;
    const double epb = 0.37 * bw;
    const double esb = std::max(0.13 * kKernelLen * bw, epb + 0.10 * bw); // 过渡带不退化
    const double dF = esb - epb;
    constexpr double kAttenDb = 68.0; // 阻带深度 (= 原版实测远端底 ≈ −70dB)
    wl = std::max(9, (int)std::ceil((kAttenDb - 8.0) / (2.285 * 2.0 * kPi * dF / fsL)));
    if ((wl & 1) == 0)
      ++wl; // 奇长 → 实对称

    // 1) 实对称原型 (中心 mid): Kaiser 窗化 sinc 低通 (DC 增益 1)
    const int mid = (wl - 1) / 2;
    std::vector<double> proto(wl);
    {
      const double beta = 0.1102 * (68.0 - 8.7);
      const double fcLp = 0.5 * (epb + esb);
      const double wC = 2.0 * kPi * fcLp / fsL; // 原型截止 (rad/样本)
      for (int n = 0; n < wl; ++n) {
        const double d = n - mid;
        const double h = (d == 0) ? (wC / kPi) : std::sin(wC * d) / (kPi * d);
        const double t = 2.0 * n / (wl - 1) - 1.0; // −1..1
        const double arg = beta * std::sqrt(std::max(0.0, 1.0 - t * t));
        const double wk = BesselI0(arg) / BesselI0(beta);
        proto[n] = h * wk;
      }
    }

    // 2) 最小相位谱分解 (固有): |G(ω)|=|A(ω)| 全频带逐点成立 (静态响应/零陷不变),
    //    能量前置 —— 起振群延迟塌缩; 核长不变 (自相关法, 见 detail::MinPhaseFactor 注)
    proto = detail::MinPhaseFactor(proto);
    // 时间反转: 点积按 "kernel[0] 配最旧样本" 取向, 卷积等效于时间翻转核 —— 不反转
    // 的话最小相位核等效成最大相位 (能量落在旧样本侧, 起振反而最慢)。反转后能量
    // 前置的抽头配最新样本, 新信号一到立即响应; 幅度响应不受翻转影响 (静态等价)。
    std::reverse(proto.begin(), proto.end());

    Band bd;
    bd.layer = L;
    bd.winLen = (int)proto.size();
    // 跨层延迟对齐: 段顶 band (fSeg=0) 对齐到下一深层基准时刻, 段底 (fSeg=1) 对齐到本层
    const double gdL = ChainGd(L);
    const double gdD = (nextDeep > 0) ? ChainGd(nextDeep) : gdL;
    const double R = gdD + (gdL - gdD) * fSeg;
    // 夹非负: 负 readOff 会让取窗起点越过缓冲区末端 (读越界)
    bd.readOff = std::max(0, (int)std::lround((R - gdL) / (double)(1 << L)));
    // 3) 载波调制 + 归一 (谱分解保持 DC 增益: Σg = A(0) = Σh, 按分解结果实测求和)
    bd.kernelRe.resize(bd.winLen);
    bd.kernelIm.resize(bd.winLen);
    const double step = 2.0 * kPi * fc / fsL; // 该层速率的归一化频率
    double ksum = 0.0;
    for (int n = 0; n < bd.winLen; ++n) {
      const double ph = step * n;
      bd.kernelRe[n] = (float)(proto[n] * std::cos(ph));
      bd.kernelIm[n] = (float)(proto[n] * std::sin(ph));
      ksum += proto[n];
    }
    bd.wsumInv = (ksum > 1e-12) ? (float)(2.0 / ksum) : 0.0f;

    mBands.push_back(std::move(bd));
    mFreqs.push_back(fc);
    mMaxWinPerLayer[L] = std::max(mMaxWinPerLayer[L], bd.winLen + bd.readOff);
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

  // 第一类修正贝塞尔 I0 (Kaiser 窗用; 级数展开, x ≤ ~10 收敛快)
  static double BesselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 32; ++k) {
      term *= (x / 2.0) / k * (x / 2.0) / k;
      sum += term;
      if (term < 1e-12 * sum)
        break;
    }
    return sum;
  }

  static constexpr double kKernelLen = 4.6; // 核长系数 k (Kaiser 墙位 Esb=0.13·k·bw), 原版拟合定值
  double mSampleRate = 48000.0;
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
