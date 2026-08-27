// paz_iir_bench.cpp — PAZ IIR 滤波器组新旧实现对照基准
//   旧: 逐 band 外层 / 样本内层 (AoS 状态, 每 band 一条串行递推链)
//   新: 样本外层 / 同层 band 内层 (SoA 状态, 各 band 递推链相互独立)
// 验证:
//   1) 数值一致性: 相同输入下两实现的逐帧带通峰值一致 (预期位级相等, 允许极小浮点漂移)
//   2) 吞吐对比: 固定工作量的墙钟时间比 (近似 UI 线程 PrepareDataForUI 内核占比)
// 说明: 基准不搭半带金字塔, 直接按层步距抽取公共信号作为各层源 —— 只影响频谱形状,
//       不影响两实现的相对耗时与逐 band 运算序列。
// 构建: c++ -std=c++17 -O2 -o paz_iir_bench paz_iir_bench.cpp && ./paz_iir_bench

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

static constexpr int kHop = 1024;
static constexpr int kMaxLayers = 10;
static constexpr double kFs = 48000.0;

static constexpr double kPazFreqs40[52] = {
    23.0,    70.0,    117.0,   164.0,   211.0,   258.0,   305.0,   352.0,
    422.0,   516.0,   609.0,   703.0,   797.0,   891.0,   984.0,   1078.0,
    1219.0,  1406.0,  1594.0,  1781.0,  1969.0,  2156.0,  2344.0,  2531.0,
    2719.0,  2906.0,  3188.0,  3563.0,  3937.0,  4312.0,  4687.0,  5063.0,
    5437.0,  5813.0,  6187.0,  6563.0,  6938.0,  7313.0,  7875.0,  8625.0,
    9375.0,  10125.0, 10875.0, 11625.0, 12750.0, 14250.0, 15750.0, 17250.0,
    18750.0, 20250.0, 21750.0, 23250.0};

struct Coefs {
  float a1, a2, a3, kb;
  int layer;
};

struct LayerSeg {
  int start, count, layer;
};

static std::vector<Coefs> BuildCoefs() {
  // 与 PAZAnalyzer::RebuildBands 默认档完全一致的公式 (fs=48000, 表 kPazFreqs40)
  constexpr double kPi = 3.14159265358979323846;
  static constexpr double kGuard = 0.38;
  const double safeLimit = kGuard * kFs;
  std::vector<Coefs> out;
  out.reserve(52);
  for (int i = 0; i < 52; ++i) {
    const double fc = kPazFreqs40[i];
    const double bw =
        (i == 0) ? (kPazFreqs40[1] - kPazFreqs40[0])
                 : (i == 51) ? (kPazFreqs40[51] - kPazFreqs40[50])
                             : 0.5 * (kPazFreqs40[i + 1] - kPazFreqs40[i - 1]);
    const double Q = std::clamp(fc / std::max(bw, 1.0), 0.707, 15.0);
    int layer = 0;
    if (fc < safeLimit) {
      layer = (int)std::floor(std::log2(safeLimit / fc));
      layer = std::clamp(layer, 0, kMaxLayers - 1);
    }
    const double layerFs = kFs / (double)(1 << layer);
    const double g = std::tan(kPi * fc / layerFs);
    const double k = 1.0 / Q;
    const double a1 = 1.0 / (1.0 + g * (g + k));
    Coefs c;
    c.a1 = (float)a1;
    c.a2 = (float)(g * a1);
    c.a3 = (float)(g * g * a1);
    c.kb = (float)(1.0 / (Q * Q));
    c.layer = layer;
    out.push_back(c);
  }
  return out;
}

static std::vector<LayerSeg> BuildSegs(const std::vector<Coefs> &cs) {
  std::vector<LayerSeg> segs;
  for (int i = 0; i < (int)cs.size();) {
    const int l = cs[i].layer;
    int j = i;
    while (j < (int)cs.size() && cs[j].layer == l)
      ++j;
    segs.push_back({i, j - i, l});
    i = j;
  }
  return segs;
}

// ---- 旧实现: 逐 band 外层, 样本内层 (AoS 状态), 与重构前的 PAZAnalyzer 完全相同 ----
struct StateAoS {
  float ic1_1, ic2_1, ic1_2, ic2_2;
};

static void ScalarBank(const std::vector<Coefs> &cs, const float *const *srcByLayer,
                       int frame, std::vector<StateAoS> &st, float *pkOut) {
  const int nb = (int)cs.size();
  for (int b = 0; b < nb; ++b) {
    const Coefs &coef = cs[b];
    const int nSamples = kHop >> coef.layer;
    const float *src = srcByLayer[coef.layer] + (size_t)frame * nSamples;

    float ic1_1 = st[b].ic1_1, ic2_1 = st[b].ic2_1;
    float ic1_2 = st[b].ic1_2, ic2_2 = st[b].ic2_2;
    float pk = 0.f;

    for (int s = 0; s < nSamples; ++s) {
      const float inSample = src[s];

      const float v3_1 = inSample - ic2_1;
      const float v1_1 = coef.a1 * ic1_1 + coef.a2 * v3_1;
      const float v2_1 = ic2_1 + coef.a2 * ic1_1 + coef.a3 * v3_1;
      ic1_1 = 2.f * v1_1 - ic1_1;
      ic2_1 = 2.f * v2_1 - ic2_1;

      const float v3_2 = v1_1 - ic2_2;
      const float v1_2 = coef.a1 * ic1_2 + coef.a2 * v3_2;
      const float v2_2 = ic2_2 + coef.a2 * ic1_2 + coef.a3 * v3_2;
      ic1_2 = 2.f * v1_2 - ic1_2;
      ic2_2 = 2.f * v2_2 - ic2_2;

      const float mag = std::abs(v1_2) * coef.kb;
      if (mag > pk)
        pk = mag;
    }

    if (std::abs(ic1_1) < 1e-30f) {
      ic1_1 = ic2_1 = ic1_2 = ic2_2 = 0.f;
    }
    st[b].ic1_1 = ic1_1;
    st[b].ic2_1 = ic2_1;
    st[b].ic1_2 = ic1_2;
    st[b].ic2_2 = ic2_2;
    pkOut[b] = pk;
  }
}

// ---- 新实现: 单层批处理核, 与 PAZAnalyzer::ProcessIirSeg 逐行相同 ----
static void ProcessIirSeg(const float *__restrict src, int nSamples,
                          const float *__restrict a1, const float *__restrict a2,
                          const float *__restrict a3, const float *__restrict kb,
                          float *__restrict ic1a, float *__restrict ic2a,
                          float *__restrict ic1b, float *__restrict ic2b,
                          float *__restrict pk, int n) {
  for (int s = 0; s < nSamples; ++s) {
    const float x = src[s];
    for (int i = 0; i < n; ++i) {
      float c1 = ic1a[i], c2 = ic2a[i];
      const float v3_1 = x - c2;
      const float v1_1 = a1[i] * c1 + a2[i] * v3_1;
      const float v2_1 = c2 + a2[i] * c1 + a3[i] * v3_1;
      c1 = 2.f * v1_1 - c1;
      c2 = 2.f * v2_1 - c2;

      float c3 = ic1b[i], c4 = ic2b[i];
      const float v3_2 = v1_1 - c4;
      const float v1_2 = a1[i] * c3 + a2[i] * v3_2;
      const float v2_2 = c4 + a2[i] * c3 + a3[i] * v3_2;
      c3 = 2.f * v1_2 - c3;
      c4 = 2.f * v2_2 - c4;
      ic1a[i] = c1;
      ic2a[i] = c2;
      ic1b[i] = c3;
      ic2b[i] = c4;

      const float mag = std::abs(v1_2) * kb[i];
      if (mag > pk[i])
        pk[i] = mag;
    }
  }
}

static void LayeredBank(const std::vector<LayerSeg> &segs, const float *const *srcByLayer,
                        int frame, const std::vector<float> &A1, const std::vector<float> &A2,
                        const std::vector<float> &A3, const std::vector<float> &KB,
                        std::array<std::vector<float>, 4> &st /* ic1a,ic2a,ic1b,ic2b */,
                        std::vector<float> &pk) {
  for (const LayerSeg &seg : segs) {
    const int ns = kHop >> seg.layer;
    ProcessIirSeg(srcByLayer[seg.layer] + (size_t)frame * ns, ns,
                  A1.data() + seg.start, A2.data() + seg.start, A3.data() + seg.start,
                  KB.data() + seg.start, st[0].data() + seg.start,
                  st[1].data() + seg.start, st[2].data() + seg.start,
                  st[3].data() + seg.start, pk.data() + seg.start, seg.count);
    for (int i = 0; i < seg.count; ++i) {
      if (std::abs(st[0][seg.start + i]) < 1e-30f) {
        st[0][seg.start + i] = 0.f;
        st[1][seg.start + i] = 0.f;
        st[2][seg.start + i] = 0.f;
        st[3][seg.start + i] = 0.f;
      }
    }
  }
}

static long long FrameCpuWork(const std::vector<LayerSeg> &segs) {
  // 每帧的样本通过量 (与运算量成正比, 用于归一化展示)
  long long pass = 0;
  for (const LayerSeg &s : segs)
    pass += (long long)s.count * (kHop >> s.layer);
  return pass;
}

int main(int argc, char **argv) {
  const std::vector<Coefs> cs = BuildCoefs();
  const std::vector<LayerSeg> segs = BuildSegs(cs);
  const int nb = (int)cs.size();

  if (argc > 1 && argv[1][0] == 'd') {
    // 调试探针: 层 0 的 band 41, 单样本逐步对照中间量
    const int b = 41;
    const Coefs &coef = cs[b];
    const int ns = kHop >> coef.layer;
    std::vector<float> x(ns);
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    for (auto &v : x)
      v = u(rng);

    float aA[1] = {coef.a1}, aB[1] = {coef.a2}, aC[1] = {coef.a3}, kb1[1] = {coef.kb};
    float soa[4][1] = {{0}, {0}, {0}, {0}};   // ic1a ic2a ic1b ic2b
    float pkD = 0.f;

    float ic1_1 = 0, ic2_1 = 0, ic1_2 = 0, ic2_2 = 0;  // 标量参照状态
    printf("   s | scalar ic2_1      soa ic2a | scalar |v1_2|*kb  soa pk\n");
    for (int s = 0; s < 16; ++s) {
      const float ix = x[s];
      const float v3_1 = ix - ic2_1;
      const float v1_1 = coef.a1 * ic1_1 + coef.a2 * v3_1;
      const float v2_1 = ic2_1 + coef.a2 * ic1_1 + coef.a3 * v3_1;
      ic1_1 = 2.f * v1_1 - ic1_1;
      ic2_1 = 2.f * v2_1 - ic2_1;
      const float v3_2 = v1_1 - ic2_2;
      const float v1_2 = coef.a1 * ic1_2 + coef.a2 * v3_2;
      const float v2_2 = ic2_2 + coef.a2 * ic1_2 + coef.a3 * v3_2;
      ic1_2 = 2.f * v1_2 - ic1_2;
      ic2_2 = 2.f * v2_2 - ic2_2;
      const float scPk = std::abs(v1_2) * coef.kb;

      ProcessIirSeg(&x[s], 1, aA, aB, aC, kb1, soa[0], soa[1], soa[2], soa[3], &pkD, 1);
      printf("%4d | %11.4e %11.4e | %11.4e %11.4e %s\n", s, ic2_1, soa[1][0], scPk, pkD,
             (memcmp(&scPk, &pkD, 4) ? " <-- MISMATCH" : ""));
    }
    return 0;
  }
  if (argc > 1 && argv[1][0] == 'D') {
    // 第 0 帧 / 第 0~2 帧逐 band 差异表
    std::vector<StateAoS> stA(nb, StateAoS{0.f, 0.f, 0.f, 0.f});
    std::array<std::vector<float>, 4> stB;
    std::vector<float> A1(nb), A2(nb), A3(nb), KB(nb);
    for (int i = 0; i < nb; ++i) {
      A1[i] = cs[i].a1;
      A2[i] = cs[i].a2;
      A3[i] = cs[i].a3;
      KB[i] = cs[i].kb;
    }
    for (auto &v : stB)
      v.assign(nb, 0.f);
    std::vector<float> sig((size_t)3 * kHop);
    std::mt19937 rng(1234567);
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    float lp = 0.f;
    for (size_t i = 0; i < sig.size(); ++i) {
      const float w = u(rng);
      lp += 0.08f * (w - lp);
      sig[i] = lp + 0.3f * w;
    }
    std::vector<std::vector<float>> src(kMaxLayers);
    std::array<const float *, kMaxLayers> srcArr{};
    for (const LayerSeg &s : segs) {
      const size_t len = (size_t)3 * (kHop >> s.layer);
      src[s.layer].resize(len);
      const int d = 1 << s.layer;
      for (size_t m = 0; m < len; ++m)
        src[s.layer][m] = sig[m * d];
      srcArr[s.layer] = src[s.layer].data();
    }
    std::vector<float> pkA(nb), pkB(nb);
    for (int f = 0; f < 3; ++f) {
      ScalarBank(cs, srcArr.data(), f, stA, pkA.data());
      LayeredBank(segs, srcArr.data(), f, A1, A2, A3, KB, stB, pkB);
      printf("-- 帧 %d --\n", f);
      for (int b = 0; b < nb; ++b)
        if (memcmp(&pkA[b], &pkB[b], 4) != 0)
          printf("band %d (层%d): 旧 %.6e 新 %.6e\n", b, cs[b].layer, pkA[b], pkB[b]);
    }
    return 0;
  }

  printf("=== PAZ IIR 对照基准 (nb=%d, %zu 层段) ===\n", nb, segs.size());
  for (const LayerSeg &s : segs)
    printf("  层 %d: band [%d..%d) 每帧 %d 样本\n", s.layer, s.start, s.start + s.count,
           kHop >> s.layer);
  printf("每帧样本通过量: %lld\n", FrameCpuWork(segs));

  // 系数 SoA 化 (与插件内 rebuild 一致)
  std::vector<float> A1(nb), A2(nb), A3(nb), KB(nb);
  for (int i = 0; i < nb; ++i) {
    A1[i] = cs[i].a1;
    A2[i] = cs[i].a2;
    A3[i] = cs[i].a3;
    KB[i] = cs[i].kb;
  }

  // ---- 输入流: 公共噪声信号按层步距抽取, 两实现共享同一份 ----
  constexpr int kFrames = 400;                     // 总帧数 (~8.5s @48k)
  std::vector<float> sig((size_t)kFrames * kHop);  // 全速率源
  {
    std::mt19937 rng(1234567);
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    float lp = 0.f;
    for (size_t i = 0; i < sig.size(); ++i) {  // 类粉感: 一阶低通白噪
      const float w = u(rng);
      lp += 0.08f * (w - lp);
      sig[i] = lp + 0.3f * w;
    }
  }
  // srcArr[l][f * (kHop>>l) .. ] 为第 f 帧该层的样本段
  std::vector<std::vector<float>> src(kMaxLayers);
  std::array<const float *, kMaxLayers> srcArr{};
  for (int l = 0; l < kMaxLayers; ++l) {
    bool used = false;
    for (const LayerSeg &s : segs)
      if (s.layer == l)
        used = true;
    if (!used)
      continue;
    const size_t len = (size_t)kFrames * (kHop >> l);
    src[l].resize(len);
    const int d = 1 << l;
    for (size_t m = 0; m < len; ++m)
      src[l][m] = sig[m * d];
    srcArr[l] = src[l].data();
  }

  // 两套独立状态 (均为零初值)
  std::vector<StateAoS> stA(nb, StateAoS{0.f, 0.f, 0.f, 0.f});
  std::array<std::vector<float>, 4> stB;
  for (auto &v : stB)
    v.assign(nb, 0.f);
  std::vector<float> pkA(nb), pkB(nb);

  // ---- 数值一致性检查: 前 kCheck 帧逐 band 对比 ----
  constexpr int kCheck = 120;
  double maxDiff = 0.0;
  int bitMismatches = 0;
  for (int f = 0; f < kCheck; ++f) {
    ScalarBank(cs, srcArr.data(), f, stA, pkA.data());
    LayeredBank(segs, srcArr.data(), f, A1, A2, A3, KB, stB, pkB);
    for (int b = 0; b < nb; ++b) {
      maxDiff = std::max(maxDiff, (double)std::abs(pkA[b] - pkB[b]));
      if (memcmp(&pkA[b], &pkB[b], sizeof(float)) != 0)
        ++bitMismatches;
    }
  }
  printf("数值一致性 (%d 帧): 最大绝对差 = %.3e, 位级不一致 band·帧 数 = %d/%d\n",
         kCheck, maxDiff, bitMismatches, kCheck * nb);

  // ---- 吞吐计时: 各自预热后, 多轮交替取最小值 ----
  auto runFrames = [&](bool layered, int f0, int f1) {
    for (int f = f0; f < f1; ++f) {
      if (layered)
        LayeredBank(segs, srcArr.data(), f, A1, A2, A3, KB, stB, pkB);
      else
        ScalarBank(cs, srcArr.data(), f, stA, pkA.data());
    }
  };
  const int kWarm = 40, kTimed = 240;
  runFrames(false, 0, kWarm);
  runFrames(true, 0, kWarm);
  double bestOld = 1e30, bestNew = 1e30;
  for (int sweep = 0; sweep < 7; ++sweep) {
    const auto t0 = std::chrono::steady_clock::now();
    runFrames(false, 0, kTimed);
    const auto t1 = std::chrono::steady_clock::now();
    runFrames(true, 0, kTimed);
    const auto t2 = std::chrono::steady_clock::now();
    const double msOld =
        std::chrono::duration<double, std::milli>(t1 - t0).count() / kTimed * 1000.0;
    const double msNew =
        std::chrono::duration<double, std::milli>(t2 - t1).count() / kTimed * 1000.0;
    bestOld = std::min(bestOld, msOld);
    bestNew = std::min(bestNew, msNew);
  }
  printf("单帧耗时 (µs): 旧 %.1f | 新 %.1f | 加速比 %.2fx\n", bestOld, bestNew,
         bestNew > 0 ? bestOld / bestNew : 0.0);
  return 0;
}
