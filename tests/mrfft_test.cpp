// mrfft_test.cpp — MultirateFFTAnalyzer 离线自测 (真实引擎, 零 iPlug 依赖)
// 验证:
//   1) 带内能量池化提取的单音保真: band 中心/两 band 中点读数 ≈ 输入幅度 (各层抽查)
//   2) 噪声类输入的 band 间平滑度: 曲率 σ 与 8dB 局部尖峰计数 (对比单 bin 采样的锯齿)
// 构建: c++ -std=c++17 -O2 -o mrfft_test mrfft_test.cpp && ./mrfft_test

#define STANDALONE_TEST 1

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

// ── iPlug 桩 (仅覆盖引擎用到的面) ──
#define BEGIN_IPLUG_NAMESPACE namespace iplug {
#define END_IPLUG_NAMESPACE }
namespace iplug {
using sample = float;
constexpr int kNoTag = -1;

struct WDL_FFT_COMPLEX {
  double re, im;
};

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

protected:
  virtual void PrepareDataForUI(Data &) {}
};
} // namespace iplug

// WDL FFT 桩: 标准基-2 FFT, 输出自然顺序, permute 恒等
void WDL_fft_init() {}

void WDL_fft(iplug::WDL_FFT_COMPLEX *buf, int n, bool inverse) {
  // 位反转置换
  for (int i = 1, j = 0; i < n; ++i) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j) {
      std::swap(buf[i].re, buf[j].re);
      std::swap(buf[i].im, buf[j].im);
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const double ang = (inverse ? 2.0 : -2.0) * M_PI / len;
    for (int i = 0; i < n; i += len) {
      double wr = 1.0, wi = 0.0;
      const double cr = std::cos(ang), ci = std::sin(ang);
      for (int k = 0; k < len / 2; ++k) {
        const double ur = buf[i + k].re, ui = buf[i + k].im;
        const double vr = buf[i + k + len / 2].re, vi = buf[i + k + len / 2].im;
        const double tr = wr * vr - wi * vi;
        const double ti = wr * vi + wi * vr;
        buf[i + k].re = ur + tr;
        buf[i + k].im = ui + ti;
        buf[i + k + len / 2].re = ur - tr;
        buf[i + k + len / 2].im = ui - ti;
        const double nwr = wr * cr - wi * ci;
        wi = wr * ci + wi * cr;
        wr = nwr;
      }
    }
  }
}

int WDL_fft_permute(int, int i) { return i; }

#include "../plugins/Analyzer/src/dsp/MultirateFFTAnalyzer.h"

using Engine = iplug::MultirateFFTAnalyzer<3, 64, 8192>;
using Data = Engine::Data;

static constexpr double kFs = 48000.0;
static constexpr int kHop = 1024;

// 逐 hop 驱动真实引擎, 返回最后一帧的 band 幅度
static std::vector<float> RunEngine(const std::vector<float> &signal) {
  Engine eng;
  std::vector<float> last;
  const int hops = (int)(signal.size() / kHop);
  for (int h = 0; h < hops; ++h) {
    Data d{iplug::kNoTag, 1, 0};
    std::memcpy(d.vals[0].data(), signal.data() + (size_t)h * kHop, kHop * sizeof(float));
    eng.TestProcessHop(d);
    last.assign(d.vals[0].begin(), d.vals[0].end());
  }
  return last;
}

int main() {
  printf("=== MultirateFFTAnalyzer 能量池化提取自测 (fs=%.0f, bpo=24) ===\n", kFs);

  // ── 1) 单音保真: band 中心 / 两 band 中点, 抽查低中高三处 ──
  int failures = 0;
  auto check = [](const char *name, double v, double lo, double hi) {
    const bool ok = v >= lo && v <= hi;
    printf("[%s] %-38s 读数=%.3f (期望 %.2f..%.2f)\n", ok ? "PASS" : "FAIL", name, v, lo, hi);
    return ok;
  };
  const int kIdx[] = {30, 111, 220}; // ~47.6Hz / ~493.5Hz / ~11.5kHz
  for (int idx : kIdx) {
    for (int frac = 0; frac <= 1; ++frac) {
      const double f0 = 20.0 * std::pow(2.0, (idx + 0.5 * frac) / 24.0);
      std::vector<float> sig((int)(4.0 * kFs));
      for (size_t i = 0; i < sig.size(); ++i)
        sig[i] = (float)std::sin(2.0 * M_PI * f0 * (double)i / kFs);
      const std::vector<float> bands = RunEngine(sig);
      // 找距 f0 最近的 band (freqs 表 20·2^(k/24), 反解 k)
      const int nb = (int)bands.size();
      int best = 0;
      double bestDist = 1e9;
      for (int b = 0; b < nb; ++b) {
        const double fb = 20.0 * std::pow(2.0, (double)b / 24.0);
        const double d = std::fabs(std::log2(fb / f0));
        if (d < bestDist) {
          bestDist = d;
          best = b;
        }
      }
      (void)nb;
      char name[64];
      std::snprintf(name, sizeof(name), "单音 %.1fHz (%s) 读数", f0,
                    frac ? "两band中点" : "band中心");
      if (!check(name, bands[best], 0.85, 1.15))
        ++failures;
    }
  }

  // ── 2) 噪声平滑度 ──
  std::mt19937 rng(1234);
  std::vector<float> sig((int)(6.0 * kFs));
  std::uniform_real_distribution<float> u(-1.f, 1.f);
  for (float &x : sig)
    x = 0.3f * u(rng);
  const std::vector<float> bands = RunEngine(sig);
  std::vector<double> db(bands.size());
  for (size_t i = 0; i < bands.size(); ++i)
    db[i] = 20.0 * std::log10(std::max(bands[i], 1e-7f));
  int spikes = 0;
  for (size_t i = 2; i + 2 < db.size(); ++i)
    if (db[i] - std::max(db[i - 1], db[i + 1]) >= 8.0 &&
        db[i] - std::max(db[i - 2], db[i + 2]) >= 10.0)
      ++spikes;
  double curv = 0.0;
  int ncurv = 0;
  for (size_t i = 2; i + 2 < db.size(); ++i) {
    if (db[i - 1] > -95.0 && db[i] > -95.0 && db[i + 1] > -95.0) {
      curv += std::fabs(db[i + 1] - 2.0 * db[i] + db[i - 1]);
      ++ncurv;
    }
  }
  curv = ncurv ? curv / ncurv : 0.0;
  printf("[%s] 白噪 band 间锯齿: 尖峰数=%d (期望<=2), 曲率σ=%.2f (期望<5)\n",
         (spikes <= 2 && curv < 5.0) ? "PASS" : "FAIL", spikes, curv);
  if (!(spikes <= 2 && curv < 5.0))
    ++failures;

  printf("\n%s (failures=%d)\n", failures ? "存在失败项" : "全部通过", failures);
  return failures ? 1 : 0;
}
