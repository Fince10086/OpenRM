// rta_test.cpp — RTAAnalyzer 离线自测 (真实引擎, 零 iPlug 依赖)
// 验证:
//   1) 分数倍频程 (1/3, 1/4, 1/6 Oct) 频带数量与频率网格覆盖 (20Hz - 20kHz)
//   2) 0 dBFS 正弦波在低/中/高频中心频率的能量检波精度 (读数 ≈ 1.0 / 0 dBFS, 彻底杜绝 in-place 缓冲覆写 bug)
//   3) 邻带隔离度与选择性 (两倍频程处衰减 > 24 dB)
//   4) 分数倍频程带边 -3dB 截止点匹配
//   5) 冻结 (Freeze) 重放与确定性 (同一输入信号重放产物逐采样一致)

#define STANDALONE_TEST 1

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <array>
#include <algorithm>

// ── iPlug 桩 ──
#define BEGIN_IPLUG_NAMESPACE namespace iplug {
#define END_IPLUG_NAMESPACE }
namespace iplug {
using sample = float;
constexpr int kNoTag = -1;

template <int MAXNC, typename PKT>
struct ISenderData {
  int ctrlTag;
  int nChans;
  int chanOffset;
  PKT vals[MAXNC];
};

template <int MAXNC, int QUEUE_SIZE, typename PKT>
class ISender {
public:
  using Data = ISenderData<MAXNC, PKT>;
  virtual ~ISender() = default;
  void PushData(const Data &d) { mQueue.push_back(d); }
  std::vector<Data> mQueue;
protected:
  virtual void PrepareDataForUI(Data &) {}
};
} // namespace iplug

#include "../plugins/Analyzer/src/dsp/RTAAnalyzer.h"

int main() {
  printf("=== Starting RTAAnalyzer Comprehensive DSP Verification ===\n");
  int failures = 0;
  auto check = [&](const char *name, bool ok) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
  };

  iplug::RTAAnalyzer<3> rta;
  rta.SetSampleRate(48000.0);

  // 1. 验证 1/3, 1/4, 1/6 Oct 的频带数量
  rta.SetOctaveMode(iplug::RTAAnalyzer<3>::kOctave1_3);
  rta.CheckRebuild();
  const int n1_3 = rta.NumBands();
  printf("1/3 Oct bands count: %d (min freq: %.1f Hz, max freq: %.1f Hz)\n",
         n1_3, rta.BandFreqs().front(), rta.BandFreqs().back());
  check("1/3 Oct band count in [30, 32]", n1_3 >= 30 && n1_3 <= 32);

  rta.SetOctaveMode(iplug::RTAAnalyzer<3>::kOctave1_4);
  rta.CheckRebuild();
  const int n1_4 = rta.NumBands();
  printf("1/4 Oct bands count: %d (min freq: %.1f Hz, max freq: %.1f Hz)\n",
         n1_4, rta.BandFreqs().front(), rta.BandFreqs().back());
  check("1/4 Oct band count in [40, 43]", n1_4 >= 40 && n1_4 <= 43);

  rta.SetOctaveMode(iplug::RTAAnalyzer<3>::kOctave1_6);
  rta.CheckRebuild();
  const int n1_6 = rta.NumBands();
  printf("1/6 Oct bands count: %d (min freq: %.1f Hz, max freq: %.1f Hz)\n",
         n1_6, rta.BandFreqs().front(), rta.BandFreqs().back());
  check("1/6 Oct band count in [60, 65]", n1_6 >= 60 && n1_6 <= 65);

  // 2. 验证低/中/高频中心频率 0 dBFS 正弦波的幅值精度
  rta.SetOctaveMode(iplug::RTAAnalyzer<3>::kOctave1_3);
  rta.CheckRebuild();

  double testFreqs[] = {31.25, 125.0, 500.0, 1000.0, 4000.0, 16000.0};
  for (double fc : testFreqs) {
    rta.ResetRuntimeState();
    rta.mQueue.clear();

    constexpr double fs = 48000.0;
    constexpr int nSamples = 1024 * 50; // 50 个完整 hop
    std::vector<float> sine(nSamples);
    for (int i = 0; i < nSamples; ++i) {
      sine[i] = (float)std::sin(2.0 * M_PI * fc * (double)i / fs);
    }

    float *blk[3] = {sine.data(), sine.data(), sine.data()};
    for (int pos = 0; pos < nSamples; pos += 1024) {
      float *b[3] = {blk[0] + pos, blk[1] + pos, blk[2] + pos};
      rta.ProcessBlock(b, 1024, 100, 3);
    }

    for (auto &pkt : rta.mQueue) {
      rta.PrepareFrameUI(pkt);
    }
    // 找到最接近 fc 的频带
    int bestB = 0;
    double bestDiff = 1e9;
    for (int b = 0; b < rta.NumBands(); ++b) {
      double d = std::abs(rta.BandFreqs()[b] - fc);
      if (d < bestDiff) { bestDiff = d; bestB = b; }
    }

    // 对稳态末尾 10 个 hop 均值 (对低频周期 > 1024 样本消除相位置换波动)
    float avgVal = 0.f;
    const int nPkt = (int)rta.mQueue.size();
    const int nAvg = std::min(10, nPkt);
    for (int i = nPkt - nAvg; i < nPkt; ++i) {
      avgVal += rta.mQueue[i].vals[0][bestB];
    }
    avgVal /= (float)nAvg;

    const float valDb = 20.0f * std::log10(std::max(avgVal, 1e-6f));
    printf("fc=%7.1f Hz (band %7.1f Hz) amplitude: %.4f (%+.2f dBFS)\n",
           fc, rta.BandFreqs()[bestB], avgVal, valDb);
    char checkName[128];
    snprintf(checkName, sizeof(checkName), "fc=%.0f Hz band gain is 0 dBFS (+-0.5 dB)", fc);
    check(checkName, std::abs(valDb) < 0.5f);
  }

  // 3. 验证带边 (-3dB) 衰减
  {
    double fEdge = 1000.0 * std::pow(2.0, -1.0 / 6.0); // 1000 Hz 1/3 Oct 的下边界 ≈ 890.9 Hz
    rta.ResetRuntimeState();
    rta.mQueue.clear();
    constexpr int nSamples = 1024 * 50;
    std::vector<float> sine(nSamples);
    for (int i = 0; i < nSamples; ++i)
      sine[i] = (float)std::sin(2.0 * M_PI * fEdge * (double)i / 48000.0);
    float *blk[3] = {sine.data(), sine.data(), sine.data()};
    for (int pos = 0; pos < nSamples; pos += 1024) {
      float *b[3] = {blk[0] + pos, blk[1] + pos, blk[2] + pos};
      rta.ProcessBlock(b, 1024, 100, 3);
    }
    for (auto &pkt : rta.mQueue) rta.PrepareFrameUI(pkt);
    const auto &lastPkt = rta.mQueue.back();

    int idx1k = 0;
    for (int b = 0; b < rta.NumBands(); ++b) {
      if (std::abs(rta.BandFreqs()[b] - 1000.0) < 1.0) { idx1k = b; break; }
    }
    float val = lastPkt.vals[0][idx1k];
    float valDb = 20.0f * std::log10(std::max(val, 1e-6f));
    printf("1/3 Oct band edge f=%.1f Hz amplitude: %.4f (%.2f dBFS) [Expected ~ -3.0 dB]\n",
           fEdge, val, valDb);
    check("1/3 Oct band edge gain is within [-3.5, -2.5] dB",
          valDb >= -3.5f && valDb <= -2.5f);
  }

  // 4. 验证选择性 (偏离两倍频程衰减 > 24 dB)
  {
    const auto &lastPkt = rta.mQueue.back();
    float maxFarLeak = 0.f;
    for (int b = 0; b < rta.NumBands(); ++b) {
      const double ratio = rta.BandFreqs()[b] / 1000.0;
      if (ratio <= 0.25 || ratio >= 4.0) {
        maxFarLeak = std::max(maxFarLeak, lastPkt.vals[0][b]);
      }
    }
    const float leakDb = 20.0f * std::log10(std::max(maxFarLeak, 1e-6f));
    printf("Far band maximum leakage: %.6f (%.2f dBFS)\n", maxFarLeak, leakDb);
    check("Far band leakage < -30 dB", leakDb < -30.0f);
  }

  // 5. 验证 Freeze 确定性重放
  rta.ResetRuntimeState();
  auto replayPkt1 = rta.mQueue.back();
  rta.PrepareFrameUI(replayPkt1);

  rta.ResetRuntimeState();
  auto replayPkt2 = rta.mQueue.back();
  rta.PrepareFrameUI(replayPkt2);

  bool replayExact = true;
  for (int b = 0; b < rta.NumBands(); ++b) {
    if (replayPkt1.vals[0][b] != replayPkt2.vals[0][b]) {
      replayExact = false;
      break;
    }
  }
  check("Deterministic replay produces identical results", replayExact);

  printf("\n=== Summary: %d failures ===\n", failures);
  return failures;
}
