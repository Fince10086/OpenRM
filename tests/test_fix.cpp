#define STANDALONE_TEST 1
#include <cmath>
#include <cstdio>
#include <vector>
#include <array>
#include <algorithm>

#define BEGIN_IPLUG_NAMESPACE namespace iplug {
#define END_IPLUG_NAMESPACE }
namespace iplug {
using sample = float;
constexpr int kNoTag = -1;
template <int MAXNC, typename PKT> struct ISenderData { int ctrlTag, nChans, chanOffset; PKT vals[MAXNC]; };
template <int MAXNC, int QUEUE_SIZE, typename PKT> class ISender {
public:
  using Data = ISenderData<MAXNC, PKT>;
  virtual ~ISender() = default;
  void PushData(const Data &d) { mQueue.push_back(d); }
  std::vector<Data> mQueue;
protected:
  virtual void PrepareDataForUI(Data &) {}
};
}

#include "../plugins/Analyzer/src/dsp/RTAAnalyzer.h"

int main() {
  iplug::RTAAnalyzer<3> rta;
  rta.SetSampleRate(48000.0);
  rta.SetOctaveMode(iplug::RTAAnalyzer<3>::kOctave1_3);
  rta.CheckRebuild();

  double testFreqs[] = {20.0, 31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 4000.0, 16000.0};
  for (double fc : testFreqs) {
    rta.ResetRuntimeState();
    rta.mQueue.clear();
    const int nSamples = 48000 * 3; // 3 seconds
    std::vector<float> sine(nSamples);
    for (int i = 0; i < nSamples; ++i)
      sine[i] = (float)std::sin(2.0 * M_PI * fc * (double)i / 48000.0);
    float *blk[3] = {sine.data(), sine.data(), sine.data()};
    for (int pos = 0; pos < nSamples; pos += 1024) {
      float *b[3] = {blk[0] + pos, blk[1] + pos, blk[2] + pos};
      rta.ProcessBlock(b, 1024, 100, 3);
    }
    for (auto &pkt : rta.mQueue) {
      // simulate copy
      alignas(16) float inBuf[1024];
      std::memcpy(inBuf, pkt.vals[0].data(), 1024 * sizeof(float));
      // now let's see what PrepareDataForUI would do
      rta.PrepareFrameUI(pkt);
    }
  }
  return 0;
}
