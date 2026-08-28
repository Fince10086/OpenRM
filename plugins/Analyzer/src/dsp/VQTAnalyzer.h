#pragma once

// VQTAnalyzer — 多速率变分辨率 Q 变换 (Multirate VQT) 频谱分析引擎
// 金字塔降采样路径可在 2 种算法间切换 (SetPyramidMode, UI 按钮 PYR):
//   LIN = 线性相位 (浅层逐级 2x 半带级联 + 深层 4x 低通; 混合倍率, CPU 低, 深层延迟最大)
//   MIN = 最小相位 (逐级 2x 半带倒谱最小相位化; 延迟最低, 幅频响应与 LIN 一致)
// 各档位共享同一 band 频率表 (显示/冻结路径无需改动), 切换仅触发 RebuildBands。

#include "ISender.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

BEGIN_IPLUG_NAMESPACE

namespace detail {

// 通用抗混叠抽取器: 支持 2x 半带 (线性相位 / 最小相位) 与 4x 低通。
// 跨帧保持滤波状态; 流式输出保持全局抽取相位与尾部状态。要求 nin 为 mD 的倍数。
struct AntiAliasDec {
  static constexpr int kMaxTaps = 177;   // 2x 半带 101 / 4x 低通 161 上限
  static constexpr double kPi = 3.14159265358979323846;

  int mD = 2;                     // 抽取倍率
  int mNumTaps = 0;               // 实际抽头数
  int mGd = 0;                    // 群延迟 (该级输入采样单位; 最小相位为通带平均)
  std::array<float, kMaxTaps> mTap{};
  std::array<float, kMaxTaps - 1> mState{};
  float mWork[kMaxTaps - 1 + 4096];
  std::array<float, 65> mTauTab{}; // τ(frac) 查找表: frac = fc/输入流Nyquist ∈[0,1), 65 点线性插值

  // 频率相关群延迟 (样本@输入采样速率)。线性相位为常数; 最小相位为差分实测曲线。
  double TauAt(double frac) const {
    double f = std::clamp(frac, 0.0, 1.0);
    const double x = f * 64.0;
    const int k = std::min(63, (int)x);
    const double t0 = mTauTab[k], t1 = mTauTab[k + 1];
    return t0 + (x - k) * (t1 - t0);
  }

  void BuildTauTab(bool minPhase) {
    if (!minPhase) {
      for (int i = 0; i < 65; ++i)
        mTauTab[i] = (float)mGd;
      return;
    }
    auto phaseAt = [&](double w) {
      double r = 0.0, iq = 0.0;
      for (int k = 0; k < mNumTaps; ++k) {
        const double ph = w * k;
        r += mTap[k] * std::cos(ph);
        iq -= mTap[k] * std::sin(ph);
      }
      return std::atan2(iq, r);
    };
    // 逐点差分 + 相位展开 (每步 Δφ ≈ τ·π/64 < π, 展开安全)。τ[0] 无实际使用
    // (最深 band 的 frac ≥ ~0.4), 直接置 0 简化。
    mTauTab[0] = 0.f;
    double prev = phaseAt(kPi / 64.0);
    for (int k = 1; k <= 64; ++k) {
      const double w = kPi * k / 64.0;
      const double cur = phaseAt(w);
      double dp = cur - prev;
      while (dp > kPi) dp -= 2.0 * kPi;
      while (dp < -kPi) dp += 2.0 * kPi;
      // 真最小相位系统群延迟恒非负; 截断 FIR 在阻带/过渡带相位无物理意义,
      // 差分会出负 τ (实测低至 -38), 负 τ 会令 readOff<0 → 取窗越过缓冲区末端
      mTauTab[k] = std::max(0.f, (float)(-dp / (kPi / 64.0)));
      prev = cur;
    }
  }

  // kind: 0 = 2x 线性相位 BH 半带 (101t, 偶抽头为 0)
  //       1 = 4x BH 低通 (57t, 截止 fs/8)
  //       2 = 2x 最小相位半带 (101t, 对 kind0 做倒谱最小相位化)
  void Build(int kind) {
    if (kind == 0) {
      BuildHalfband2x(false);
      BuildTauTab(false);
    } else if (kind == 1) {
      BuildLowpass4x();
      BuildTauTab(false);
    } else {
      BuildHalfband2x(true);
      BuildTauTab(true);
    }
  }

  void BuildHalfband2x(bool minPhase) {
    constexpr int kN = 101;
    mNumTaps = kN;
    mD = 2;
    mGd = (kN - 1) / 2;
    double taps[kN];
    double sum = 0.0;
    for (int i = 0; i < kN; ++i) {
      const int n = i - (kN - 1) / 2;
      double v;
      if (n == 0)
        v = 0.5;
      else if ((n & 1) == 0)
        v = 0.0;                                  // 偶序 (除中心) = 0: 半带结构
      else
        v = 0.5 * std::sin(kPi * n / 2.0) / (kPi * n / 2.0);
      const double theta = 2.0 * kPi * i / (kN - 1);
      v *= 0.35875 - 0.48829 * std::cos(theta)
                   + 0.14128 * std::cos(2.0 * theta)
                   - 0.01168 * std::cos(3.0 * theta);
      taps[i] = v;
      sum += v;
    }
    for (int i = 0; i < kN; ++i)
      mTap[i] = (float)(taps[i] / sum);
    if (minPhase)
      MinPhaseConvert(mTap, kN, mGd);
  }

  void BuildLowpass4x() {
    constexpr int kN = 161;
    constexpr double kCut = 0.112;                // 截止/fs_in (旧 57t@0.125 过渡带过宽导致泄漏)
    mNumTaps = kN;
    mD = 4;
    mGd = (kN - 1) / 2;
    double taps[kN];
    double sum = 0.0;
    for (int i = 0; i < kN; ++i) {
      const int n = i - (kN - 1) / 2;
      const double x = (n == 0) ? 1.0 : std::sin(2.0 * kPi * kCut * n) / (2.0 * kPi * kCut * n);
      double v = 2.0 * kCut * x;
      const double theta = 2.0 * kPi * i / (kN - 1);
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

  // 通用 FIR + mD 抽取 (out 可与 in 别名, 输入先拷贝到 mWork)
  int Process(const float* in, int nin, float* out) {
    const int nt = mNumTaps;
    const int sz = (nt - 1) + nin;
    for (int i = 0; i < nt - 1; ++i)
      mWork[i] = mState[i];
    for (int i = 0; i < nin; ++i)
      mWork[(nt - 1) + i] = in[i];
    const int nout = nin / mD;
    for (int j = 0; j < nout; ++j) {
      const int p = mD * j;
      float acc = 0.f;
      for (int i = 0; i < nt; ++i)
        acc += mTap[i] * mWork[(nt - 1) + p - i];
      out[j] = acc;
    }
    for (int i = 0; i < nt - 1; ++i)
      mState[i] = mWork[sz - (nt - 1) + i];
    return nout;
  }

  // 基 2 FFT (原位, n 为 2 的幂; forward 无归一, inverse 除 n)
  static void Fft(int n, double* re, double* im, bool inverse) {
    for (int i = 1, j = 0; i < n; ++i) {
      int bit = n >> 1;
      for (; j & bit; bit >>= 1)
        j ^= bit;
      j ^= bit;
      if (i < j) {
        std::swap(re[i], re[j]);
        std::swap(im[i], im[j]);
      }
    }
    for (int len = 2; len <= n; len <<= 1) {
      const double ang = (inverse ? 2.0 : -2.0) * kPi / len;
      const double wr0 = std::cos(ang), wi0 = std::sin(ang);
      for (int i = 0; i < n; i += len) {
        double wr = 1.0, wi = 0.0;
        for (int k = 0; k < len / 2; ++k) {
          const double ur = re[i + k], ui = im[i + k];
          const double vr = re[i + k + len / 2], vi = im[i + k + len / 2];
          const double tr = wr * vr - wi * vi;
          const double ti = wr * vi + wi * vr;
          re[i + k] = ur + tr;
          im[i + k] = ui + ti;
          re[i + k + len / 2] = ur - tr;
          im[i + k + len / 2] = ui - ti;
          const double nwr = wr * wr0 - wi * wi0;
          wi = wr * wi0 + wi * wr0;
          wr = nwr;
        }
      }
    }
    if (inverse)
      for (int i = 0; i < n; ++i) {
        re[i] /= n;
        im[i] /= n;
      }
  }

  // 倒谱法最小相位化: 保持幅度谱 (阻带不变), 群延迟降至通带平均 (~N/4)。
  // 输出群延迟写入 gdOut (通带 0.05π..0.35π 相位差分平均)。
  static void MinPhaseConvert(std::array<float, kMaxTaps>& taps, int n, int& gdOut) {
    constexpr int M = 1024;
    double re[M], im[M];
    for (int i = 0; i < M; ++i) {
      re[i] = (i < n) ? (double)taps[i] : 0.0;
      im[i] = 0.0;
    }
    Fft(M, re, im, false);
    for (int i = 0; i < M; ++i) {
      const double m = std::max(std::hypot(re[i], im[i]), 1e-12);
      re[i] = std::log(m);                        // log|H| (偶对称)
      im[i] = 0.0;
    }
    Fft(M, re, im, true);                         // 倒谱 c
    for (int i = 1; i < M / 2; ++i)
      re[i] *= 2.0;                               // 最小相位截断
    for (int i = M / 2 + 1; i < M; ++i)
      re[i] = 0.0;
    Fft(M, re, im, false);                        // FFT(c_min)
    for (int i = 0; i < M; ++i) {
      const double e = std::exp(re[i]);
      const double r = e * std::cos(im[i]);
      im[i] = e * std::sin(im[i]);
      re[i] = r;
    }
    Fft(M, re, im, true);                         // h_min
    double dc = 0.0;
    for (int i = 0; i < n; ++i)
      dc += re[i];
    for (int i = 0; i < n; ++i)
      taps[i] = (float)(re[i] / dc);              // DC 增益归一
    // 通带群延迟 (逐点相位差分 + 展开平均, 0.05π..0.35π; 每步 Δφ ≈ τ·Δw < π)
    auto phaseAt = [&](double w) {
      double r = 0.0, iq = 0.0;
      for (int k = 0; k < n; ++k) {
        const double ph = w * k;
        r += taps[k] * std::cos(ph);
        iq -= taps[k] * std::sin(ph);
      }
      return std::atan2(iq, r);
    };
    constexpr int kPts = 24;
    double prev = phaseAt(kPi * 0.05);
    double acc = 0.0;
    for (int kp = 1; kp <= kPts; ++kp) {
      const double w = kPi * (0.05 + 0.30 * kp / kPts);
      const double cur = phaseAt(w);
      double dp = cur - prev;
      while (dp > kPi) dp -= 2.0 * kPi;
      while (dp < -kPi) dp += 2.0 * kPi;
      acc += dp;
      prev = cur;
    }
    const double gd = -acc / (kPi * 0.30);
    gdOut = std::max(1, (int)std::lround(gd));
  }
};

} // namespace detail

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

  // 金字塔算法档位 (UI 按钮 PYR 二选一)
  static constexpr int kPyramidLin = 0; // 线性相位: 浅层 2x 半带 + 深层 4x 低通 (混合倍率)
  static constexpr int kPyramidMin = 1; // 最小相位: 逐级 2x (倒谱谱因式分解, 延迟最低)

  VQTAnalyzer() {
    for (int c = 0; c < MAXNC; ++c) {
      mPending[c].assign(kHop, 0.f);
      mLayers[c].resize(kMaxLayers);
    }
    RebuildBands();
  }

  // 返回是否实际请求了重建 (参数与当前值相同则返回 false)。调用方据此决定
  // 是否需要通知显示层重置。实际重建发生在 UI 线程 (CheckRebuild/PrepareDataForUI)。
  // 旧状态文件存的 0/1/2 (A/B1/B2) 由 clamp 自然迁移: 2(MIN) → 1, 0/1 → LIN。
  bool SetPyramidMode(int mode) {
    const int v = std::clamp(mode, 0, 1);
    if (v != mPyramid) {
      mPyramid = v;
      mNeedRebuild.store(true);
      return true;
    }
    return false;
  }

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

  bool SetWindowType(int windowType) {
    const int w = std::clamp(windowType, 0, 1);
    if (w != mWindowType) {
      mWindowType = w;
      mNeedRebuild.store(true);
      return true;
    }
    return false;
  }

  int GetWindowType() const { return mWindowType; }

  void SetChannelMode(int chanTri) {
    mChanTri = std::clamp(chanTri, 0, 2);
  }

  // UI 线程: 若音频线程请求了重建 (γ/BPO/采样率/金字塔档位变化), 惰性重建 band 表与金字塔。
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

  // Freeze (冻结) 支持: 复位分析侧运行态 (band 相位/层缓冲/抽取器), 不动输入侧
  // mBufCount/mPending 与 band 表 —— 冻结回放的确定性起点 (等价于引擎冷启动)
  void ResetRuntimeState() {
    for (int c = 0; c < MAXNC; ++c) {
      mRun[c].assign((size_t)mBands.size(), RunState{0, 0.f});
      for (int l = 0; l < kMaxLayers; ++l)
        mLayers[c][l].clear();
      for (int l = 0; l < kMaxLayers; ++l)
        mDecim[c][l].Reset();
    }
  }

  // Freeze (冻结) 支持: 输入侧 hop 相位查询 (音频线程 pending 计数, 冻结回放帧格对齐用)
  int HopPhase() const { return mBufCount; }

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

      // 金字塔降采样 (按当前档位的路径抽取)
      AppendLayer(c, 0, raw, kHop, mMaxWinPerLayer[0]);
      const float *cur = raw;
      int nin = kHop;
      for (int s = 0; s < mNumPathSteps; ++s) {
        const DecimStep &st = mPath[s];
        float *dst = mScratch[(s & 1)].data();
        const int nout = mDecim[c][st.decimIdx].Process(cur, nin, dst);
        AppendLayer(c, st.dstLayer, dst, nout, mMaxWinPerLayer[st.dstLayer]);
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
            // 跨层延迟对齐 (readOff 夹非负: 即使上游失效也保证取窗不越界)
            const int readOff = std::max(0, bd.readOff);
            const int have = std::max(0, std::min(wl, (int)buf.size() - readOff));
            const int skip = wl - have;
            const int start = std::max(0, (int)buf.size() - readOff - have);
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
    int layer;              // 速率层
    float wsumInv;
    int winLen;             // 本层速率窗长
    int advance;
    int readOff;
    std::vector<float> kernelRe, kernelIm; // 在该层速率下计算的 w·cos/w·sin
  };

  // 逐 band 运行状态
  struct RunState {
    int phase = 0;
    float last = 0.f;
  };

  // 金字塔抽取路径一步: 由 src (隐式 = 上一步的 dst) 抽取到 dstLayer
  struct DecimStep {
    int dstLayer;
    int decimIdx;
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

  // B1: 目标层无流时向上回退到最近的带流层
  int EffectiveLayer(double bandHi, double fs) const {
    int L = AssignLayer(bandHi, fs);
    while (L > 0 && !mLayerValid[L])
      --L;
    return L;
  }

  void RebuildBands() {
    mBands.clear();
    mFreqs.clear();
    mMaxWinPerLayer.fill(0);
    const double fs = std::max(mSampleRate, 1.0);
    const double q = 1.0 / (std::pow(2.0, 1.0 / mBpo) - 1.0);
    const int mode = mPyramid;

    // ── 1. 金字塔路径 / 有效层 ──
    mLayerValid.fill(1);
    int nSteps = 0;
    if (mode == kPyramidLin) {
      // 浅层逐级 2x (L0→L5), 深层 4x 一次合并两级 (L5→L7, L7→L9)
      for (int l = 0; l <= 4; ++l)
        mPath[nSteps++] = {l + 1, l};
      mPath[nSteps++] = {7, 5};
      mPath[nSteps++] = {9, 6};
      mLayerValid[6] = 0;
      mLayerValid[8] = 0;
    } else {
      for (int l = 0; l < kMaxLayers - 1; ++l)
        mPath[nSteps++] = {l + 1, l};
    }
    mNumPathSteps = nSteps;

    // 抽取器 (LIN: 前 5 级 2x + 2 个 4x; MIN: 全 2x 最小相位)
    for (int l = 0; l < kMaxLayers; ++l) {
      int kind = 0;
      if (mode == kPyramidMin)
        kind = 2;
      else if (mode == kPyramidLin && (l == 5 || l == 6))
        kind = 1;
      for (int c = 0; c < MAXNC; ++c)
        mDecim[c][l].Build(kind);
    }

    // ── 2. band 表 (频率序; 层内连续段 + B1 上移段自然并入) ──
    struct Spec { double fc, bw; int layer; };
    std::vector<Spec> spec;
    std::array<int, kMaxLayers> layerCount{};
    for (int k = 0;; ++k) {
      const double fc = kFreqLo * std::pow(2.0, (double)k / mBpo);
      if (fc > kFreqHi)
        break;
      const double bw = fc / q + mGamma;
      const int L = EffectiveLayer(fc + bw / 2.0, fs);
      spec.push_back({fc, bw, L});
      layerCount[L]++;
    }

    for (int i = 0, n = (int)spec.size(); i < n;) {
      const int L = spec[i].layer;
      int j = i;
      while (j < n && spec[j].layer == L)
        ++j; // 本层 band 段 [i, j)

      // 下一更深带流层 (B1 跳过无流层)
      int nextDeep = -1;
      for (int d = L + 1; d < kMaxLayers; ++d)
        if (layerCount[d] > 0) {
          nextDeep = d;
          break;
        }
      for (int t = i; t < j; ++t) {
        const double f = (j - i > 1) ? (double)(t - i) / (j - i - 1) : 0.0;
        AddBand(spec[t].fc, spec[t].bw, fs, L, nextDeep, f);
      }
      i = j;
    }

    ResetRuntimeState();
  }

  // 频率相关总群延迟 (输入采样单位): 沿路径累计各级滤波器在 fc 处的 τ。
  // 线性相位模式下各级 τ 恒定 → 退化为 mGd·(2^L−1); 最小相位 (B2) 下按
  // band 中心频率精确对齐, 消除层边界群的时序错位 (割裂感)。
  double GdTotalSamples(double fc, double fs, int L) const {
    double g = 0.0;
    int src = 0;
    for (int s = 0; s < mNumPathSteps; ++s) {
      const DecimStep &st = mPath[s];
      if (st.dstLayer > L)
        break;
      const double nyqIn = fs / (double)(1 << (src + 1)); // 该级输入流 Nyquist
      g += mDecim[0][st.decimIdx].TauAt(fc / nyqIn) * (double)(1 << src);
      src = st.dstLayer;
    }
    return g;
  }

  void AddBand(double fc, double bw, double fs, int L, int nextDeep, double fSeg) {
    const double fsL = fs / (double)(1 << L);
    const int wl = std::max(8, std::max((int)std::round(fsL / bw),
                                        (int)std::round(fsL / fc * kCycleFloor)));
    Band bd;
    bd.layer = L;
    bd.winLen = wl;
    bd.advance = 1;
    // 跨层延迟对齐: 段顶 band (fSeg=0) 对齐到下一深层的基准时刻, 段底 (fSeg=1) 对齐到本层
    const double gdL = GdTotalSamples(fc, fs, L);
    const double gdD = (nextDeep > 0) ? GdTotalSamples(fc, fs, nextDeep) : gdL;
    const double R = gdD + (gdL - gdD) * fSeg;
    // 夹非负: 负 readOff 会让取窗起点越过缓冲区末端 (读越界)
    bd.readOff = std::max(0, (int)std::lround((R - gdL) / fs * fsL));
    bd.kernelRe.resize(wl);
    bd.kernelIm.resize(wl);
    const double step = 2.0 * PI * fc / fsL; // 该层速率的归一化频率
    double sum = 0.0;
    for (int n = 0; n < wl; ++n) {
      double w = 0.0;
      const double theta = 2.0 * PI * n / (wl - 1);
      if (mWindowType == 0) {
        // 0: Hann 窗 (SHARP 档): 窄主瓣, 提升相邻音符分辨力与时域起振敏锐度
        w = 0.5 * (1.0 - std::cos(theta));
      } else {
        // 1: 4-term Blackman-Harris 窗 (CLEAN 档): 阻带衰减至 -92dB, 杜绝相邻音乐频段横向泄漏
        w = 0.35875 - 0.48829 * std::cos(theta)
                    + 0.14128 * std::cos(2.0 * theta)
                    - 0.01168 * std::cos(3.0 * theta);
      }
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

  int mPyramid = 0;                       // 金字塔档位 (kPyramidLin/kPyramidMin)
  int mWindowType = 0;                    // 窗函数档位 (0=SHARP/Hann, 1=CLEAN/BH4)
  int mBpo = 24;                          // bins per octave (插件层固定 24)
  int mGamma = 5;                         // 低频带宽下限 Hz (插件层固定 HIGH 档)
  int mChanTri = 0;                       // 0=LR, 1=PWR, 2=SUM
  double mSampleRate = 48000.0;
  std::atomic<bool> mNeedRebuild{false};
  std::vector<Band> mBands;
  std::vector<double> mFreqs;
  std::array<int, kMaxLayers> mMaxWinPerLayer{};
  std::array<int, kMaxLayers> mLayerValid{};   // 该层是否有流 (B1 跳过 L6/L8)
  std::array<DecimStep, kMaxLayers> mPath{};
  int mNumPathSteps = 0;
  std::array<std::vector<std::vector<float>>, MAXNC> mLayers;
  std::array<std::array<detail::AntiAliasDec, kMaxLayers>, MAXNC> mDecim;
  std::array<std::vector<RunState>, MAXNC> mRun;
  std::array<float, kHop> mScratch[2];
  int mBufCount = 0;
  std::array<std::vector<float>, MAXNC> mPending;
};

END_IPLUG_NAMESPACE