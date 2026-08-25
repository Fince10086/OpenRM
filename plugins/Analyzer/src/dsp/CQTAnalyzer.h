#pragma once

// CQTAnalyzer — 变分辨率 CQT (VQT) 频谱分析器 (多速率直接 CQT)。
//
// 频率/带宽: IEM CQT Analyzer (AES 2020) 带宽公式 + J.C. Brown 1991 直接 CQT:
//   fk = f0·2^(k/B)          (B = bins per octave, 12/24)
//   Bk = fk/Q + γ            (γ = 低频带宽下限 Hz, 10/20/40)
//   Q  = 1/(2^(1/B) - 1),  Nk = fs/Bk  (Hann 窗 + 预计算复数内核 + 块式 hop 点积)
// 幅度归一化: /(Σw/2) 恢复输入幅度, /sqrt(Bk) 对齐 FFT power-per-bin 语义。
//
// 多速率金字塔 (关键性能优化): band 的输出带宽只有 Bk, 只需 ~2·Bk 的速率即可表达,
// 因此低频 band 不需要在 48kHz 全速率上跑 1183 点长内核 —— 那 88% 的乘加是冗余的。
// 这里用逐级 ×2 半带抽取把输入灌进 kMaxLayers 层金字塔 (48k/24k/.../~94Hz),
// 每层一个共享历史缓冲, 每个 band 按自己的层速率用短内核点积 (8~38 抽头)。
//   层分配约束: 带通上边 fc+Bk/2 必须低于该层新奈奎斯特的 kGuard 比例 (物理约束, 高频
//   band fc 高 → 无法深抽取, 于是保持全速率, 这是恒定 Q 该付出的成本而非缺陷)。
//   窗口下限: 深层速率下 winLen 会跌到几个样本, 小于 1 个 fc 周期时短窗测量不稳定,
//   因此窗长至少覆盖 kCycleFloor(≈4) 个 fc 周期 —— 深层仅 ~10~40 抽头, 仍远小于全速率。
// 半带滤波器: 17 抽头 Hamming 窗 sinc (截止 π/2, DC 增益归一), 每 ×2 级约 9 次乘加,
// 整条金字塔每帧每通道约 9k 次乘加, 相对全速率 174k 抽头可忽略。流式抽取 == 一次性
// 参考 (python 逐位验证); 带内音幅度保留 ~1.0, 带外抑制 -40dB+。
//
// 线程模型: 音频线程仅在 ProcessBlock 内采集 hop 样本入队 (与 SpectrumSTFT 一致);
// band/金字塔/各层历史全部由 UI 线程 PrepareDataForUI 独占 (OnIdle→TransmitData)。
// γ/BPO/采样率变化时音频线程只写标量并置 mNeedRebuild, 惰性重建在 UI 线程执行。
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

namespace detail {

// 半带 ×2 抽取器 (跨帧保持滤波状态)。系数为 17 抽头 Hamming 窗半带 (截止 π/2, DC 增益 1),
// 偶数序 (除中心) 抽头严格为零 → 每输出样本 9 次乘加。流式输出 = 一次性"滤波后隔点取"
// 的逐位同样 (保持全局偶数位抽取相位与尾部状态)。要求 nin 为偶数。
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

template <int MAXNC = 2, int QUEUE_SIZE = 64, int MAX_BANDS = 4096>
class CQTAnalyzer : public ISender<MAXNC, QUEUE_SIZE, std::array<float, MAX_BANDS>> {
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

  CQTAnalyzer() {
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

protected:
  // UI 线程: 弹出的数据含各通道最新 kHop 个原始样本。先灌入多速率金字塔 (各层历史
  // 追加降采样样本), 再对每个 band 从它所在层取最近 winLen 个样本做短点积, 写回
  // d.vals (TransmitData 随后发送给显示控件)。与 SpectrumSTFT 一致, 全部计算在 UI 线程。
  void PrepareDataForUI(Data &d) override {
    CheckRebuild();
    const int nb = NumBands();
    if (nb <= 0)
      return;
    const int nCh = std::min(d.nChans, MAXNC);

    for (int c = 0; c < nCh; ++c) {
      const float *raw = d.vals[c].data();

      // 金字塔: 原始样本进层 0 历史, 同时逐级降采样, 各层历史追加 (仅保留最近所需窗长)。
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

      // band 点积: 每 band 从所在层取最近 winLen 个样本
      for (int b = 0; b < nb; ++b) {
        const Band &bd = mBands[b];
        const std::vector<float> &buf = mLayers[c][bd.layer];
        const int wl = bd.winLen;
        const int have = std::min(wl, (int)buf.size()); // 实际可用样本数
        const int skip = wl - have;                     // 窗口前补零 (启动阶段)
        const float *x = buf.data() + (buf.size() - have);
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
    int layer;                    // 金字塔层号 (D = 1<<layer)
    float wsumInv;                // 4/winLen: 恢复输入幅度
    float invSqrtBw;              // 1/sqrt(Bk): 功率密度归一化
    int winLen;                   // 该层速率下窗长 (样本) = fsL/Bk (下限: ≥~4 个 fc 周期)
    std::vector<float> kernelRe, kernelIm; // 在该层速率下计算的 w·cos/w·sin
  };

  // 往层 l 历史末尾追加 n 个样本, 使缓冲只保留最近 maxKeep 个 (即该层最大窗长)。
  // n 可大于 maxKeep (如层 0 每帧灌 kHop 个原始样本), 此时只保留新批次尾部。
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

    for (int k = 0;; ++k) {
      const double fc = kFreqLo * std::pow(2.0, (double)k / mBpo);
      if (fc > kFreqHi)
        break;
      const double bw = fc / q + mGamma; // Bk: 低频段被 γ 托底, 高频段趋于恒定 Q
      AddBand(fc, bw, fs);
    }

    // 清空金字塔 (层历史 + 各级滤波状态)
    for (int c = 0; c < MAXNC; ++c) {
      for (int l = 0; l < kMaxLayers; ++l)
        mLayers[c][l].clear();
    }
    for (int c = 0; c < MAXNC; ++c)
      for (int l = 0; l < kMaxLayers; ++l)
        mDecim[c][l].Reset();
  }

  void AddBand(double fc, double bw, double fs) {
    // 层分配: 最大 L 使带通上边 fc+Bk/2 落在该层新奈奎斯特的 kGuard 比例内 (物理约束)
    const double bandHi = fc + bw / 2.0;
    int L = 0;
    while (L + 1 < kMaxLayers) {
      const double newNyq = fs / (double)(1 << (L + 2)); // 层 L+1 的奈奎斯特
      if (bandHi > kGuard * newNyq)
        break;
      ++L;
    }
    const double fsL = fs / (double)(1 << L);

    // 窗长: 1/Bk (频率分辨率) 但至少覆盖 ~kCycleFloor 个 fc 周期 (底层短窗不稳定), 且 ≥8。
    const int wl = std::max(8, std::max((int)std::round(fsL / bw),
                                        (int)std::round(fsL / fc * kCycleFloor)));

    Band bd;
    bd.layer = L;
    bd.winLen = wl;
    bd.wsumInv = (float)(4.0 / wl);
    bd.invSqrtBw = (float)(1.0 / std::sqrt(bw));
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
    mMaxWinPerLayer[L] = std::max(mMaxWinPerLayer[L], wl);
  }

  int mBpo = 24;                          // bins per octave (12/24); 音频线程写, UI 线程读
  int mGamma = 40;                        // 低频带宽下限 Hz (10/20/40)
  double mSampleRate = 48000.0;
  std::atomic<bool> mNeedRebuild{false};  // 音频线程置位, UI 线程读取并清除
  std::vector<Band> mBands;               // UI 线程独占
  std::vector<double> mFreqs;             // UI 线程独占
  std::array<int, kMaxLayers> mMaxWinPerLayer{};      // 每层需保留的最大窗长 (UI 线程独占)
  std::array<std::vector<std::vector<float>>, MAXNC> mLayers; // [c][l] 每层历史 (UI 线程独占)
  std::array<std::array<detail::HalfbandDec2, kMaxLayers>, MAXNC> mDecim; // 每通道每级滤波 (UI 线程独占)
  std::array<float, kHop> mScratch[2];    // 级联逐级降采样的交替缓冲 (UI 线程独占)
  int mBufCount = 0;                      // 音频线程独占
  std::array<std::vector<float>, MAXNC> mPending;      // 音频线程独占 (构造时分配, 不再重分配)
};

END_IPLUG_NAMESPACE
