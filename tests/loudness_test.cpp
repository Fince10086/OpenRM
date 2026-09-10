// loudness_test.cpp — LoudnessMeter 离线自测 (纯 C++17, 零 iPlug 依赖)
// 验证 (ITU-R BS.1770-4 / EBU R128 / EBU Tech 3342):
//   1) 校准: 1kHz 正弦 0 dBFS ≈ -3.01 LUFS (标准定义);
//      M/S/I 与 H(e^jω) 频率域预测一致 (独立于引擎的时域模拟)
//   2) 线性: 电平差 12 LU → 响度差 12 LU
//   3) K 加权低频滚降: 100Hz 与 1kHz 的响度差符合滤波器频响
//   4) 门限: -90dBFS 弱段被绝对门限剔除; -60dBFS 段被相对门限剔除
//   5) LRA: 双电平节目 ≈ 16 LU; 恒电平节目 ≈ 0
//   6) M/S 时间响应: 阶跃后 400ms / 3s 收敛
//   7) Reset / 采样率不变性 / TP 锁存 / 不规则宿主块长
// 构建: c++ -std=c++17 -O2 -o loudness_test loudness_test.cpp
#include "../plugins/Analyzer/src/dsp/LoudnessMeter.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace iplug;

static constexpr double kFs = 48000.0;
static int tests = 0;
static int failures = 0;

static void check(const std::string &name, bool ok) {
  std::printf("%-56s %s\n", name.c_str(), ok ? "PASS" : "FAIL");
  ++tests;
  if (!ok)
    ++failures;
}

// 滤波器 H(e^{jω}) 幅度 (dB): 由 b/a 系数直接求值, 独立于引擎时域模拟
static double FilterMagnitudeDb(const double b[5], const double a[5], double sr, double hz) {
  const double w = 2.0 * M_PI * hz / sr;
  const double c1 = std::cos(w), s1 = std::sin(w);
  double zr = 1.0, zi = 0.0; // z^{-k}, 递推 e^{-jw}
  double nr = 0.0, ni = 0.0, dr = 0.0, di = 0.0;
  for (int k = 0; k < 5; ++k) {
    nr += b[k] * zr;
    ni += b[k] * zi;
    dr += a[k] * zr;
    di += a[k] * zi;
    const double tzr = zr * c1 + zi * s1; // (zr+j·zi)·e^{-jw} = (zr·c1+zi·s1) + j(zi·c1-zr·s1)
    zi = zi * c1 - zr * s1;
    zr = tzr;
  }
  return 10.0 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

// 期望响度 (LUFS): 双声道同电平时 z = G²·2·0.5·10^(level/10) = G²·10^(level/10),
// 即 L = -0.691 + |H|²(dB) + levelDb (测 1kHz 立体声 0dBFS ≈ +0.01, 见 "1d" 注释)
static double ExpectedLufs(double sr, double hz, double levelDb) {
  double b[5], a[5];
  LoudnessMeter::KWeightCoeffs(sr, b, a);
  return -0.691 + FilterMagnitudeDb(b, a, sr, hz) + levelDb;
}

// 生成正弦 (mono=true 时只有 L 声道有声, R 全零 —— 对应标准的单声道校准点)
static void ProcessSine(LoudnessMeter &lm, double seconds, double levelDb, double freqHz,
                        int block = 512, bool mono = false, double sr = kFs) {
  const int n = (int)(seconds * sr);
  std::vector<float> L(n), R(n);
  if (freqHz > 0.0) {
    const double amp = std::pow(10.0, levelDb / 20.0);
    for (int i = 0; i < n; ++i) {
      const float v = (float)(amp * std::sin(2.0 * M_PI * freqHz * (double)i / sr));
      L[i] = v;
      R[i] = mono ? 0.0f : v;
    }
  }
  for (int pos = 0; pos < n; pos += block)
    lm.Process(L.data() + pos, R.data() + pos, std::min(block, n - pos));
}

int main() {
  // ── 1. 校准与 M/S/I 一致性 (1kHz 正弦 0 dBFS, 10s) ────────────────────────
  {
    LoudnessMeter lm;
    lm.SetSampleRate(kFs);
    ProcessSine(lm, 10.0, 0.0, 1000.0);
    LoudnessMeter::Snapshot s;
    lm.Store(s);
    const double exp0 = ExpectedLufs(kFs, 1000.0, 0.0);
    check("1a: I matches H(e^jw) prediction @1kHz 0dBFS stereo", std::fabs(s.integrated - exp0) < 0.10);
    check("1b: M/S agree with I (steady sine)",
          std::fabs(s.momentary - s.integrated) < 0.10 && std::fabs(s.shortTerm - s.integrated) < 0.10);

    // 标准校准点: 单声道 1kHz 0 dBFS → -3.01 LUFS (ITU-R BS.1770-4 §3)
    LoudnessMeter lmMono;
    lmMono.SetSampleRate(kFs);
    ProcessSine(lmMono, 10.0, 0.0, 1000.0, 512, true);
    LoudnessMeter::Snapshot sM;
    lmMono.Store(sM);
    check("1c: mono calibration: 1kHz 0dBFS = -3.01 LUFS", std::fabs(sM.integrated + 3.01) < 0.15);
    check("1d: I valid after 10s", s.iValid);
  }

  // ── 1e/1f. LRA 有效位阈值 ────────────────────────────────────────────────
  // EBU 3342 的统计参考要求 ≥60s, 本引擎按插件惯例提前: mHops >= kLraMinHops
  // (=30 hop = 3s) 即置 lraValid。此处锁定该阈值的两侧, 防止回归。
  {
    LoudnessMeter lmBefore;
    lmBefore.SetSampleRate(kFs);
    ProcessSine(lmBefore, 2.0, -20.0, 1000.0); // 2s < 3s
    LoudnessMeter::Snapshot sBefore;
    lmBefore.Store(sBefore);
    check("1e: LRA not valid before 3s", sBefore.iValid && !sBefore.lraValid);

    LoudnessMeter lmAfter;
    lmAfter.SetSampleRate(kFs);
    ProcessSine(lmAfter, 10.0, -20.0, 1000.0); // 10s > 3s (仍远早于 60s)
    LoudnessMeter::Snapshot sAfter;
    lmAfter.Store(sAfter);
    check("1f: LRA valid from 3s (plugin convention, earlier than 60s reference)",
          sAfter.iValid && sAfter.lraValid);
  }

  // ── 2. 线性: -8 与 -20 dBFS 差 12 LU ──────────────────────────────────────
  {
    LoudnessMeter lm8, lm20;
    lm8.SetSampleRate(kFs);
    lm20.SetSampleRate(kFs);
    ProcessSine(lm8, 10.0, -8.0, 1000.0);
    ProcessSine(lm20, 10.0, -20.0, 1000.0);
    LoudnessMeter::Snapshot s8, s20;
    lm8.Store(s8);
    lm20.Store(s20);
    check("2a: -20 dBFS matches prediction", std::fabs(s20.integrated - ExpectedLufs(kFs, 1000.0, -20.0)) < 0.10);
    check("2b: level step 12 LU -> loudness step 12 LU", std::fabs((s8.integrated - s20.integrated) - 12.0) < 0.05);
  }

  // ── 3. K 加权低频滚降 (50Hz / 100Hz vs 1kHz, 同电平) ────────────────────
  {
    LoudnessMeter lm100;
    lm100.SetSampleRate(kFs);
    ProcessSine(lm100, 10.0, 0.0, 100.0);
    LoudnessMeter::Snapshot s;
    lm100.Store(s);
    const double exp100 = ExpectedLufs(kFs, 100.0, 0.0);
    check("3a: 100Hz I matches prediction", std::fabs(s.integrated - exp100) < 0.10);

    LoudnessMeter lm50;
    lm50.SetSampleRate(kFs);
    ProcessSine(lm50, 10.0, 0.0, 50.0);
    LoudnessMeter::Snapshot s50;
    lm50.Store(s50);
    const double exp1k = ExpectedLufs(kFs, 1000.0, 0.0);
    const double exp50 = ExpectedLufs(kFs, 50.0, 0.0);
    check("3b: 50Hz I matches prediction", std::fabs(s50.integrated - exp50) < 0.10);
    check("3c: K-weighting rolls off at LF (50Hz >= 3 LU below 1kHz)",
          (exp1k - s50.integrated) >= 3.0 && std::fabs((exp1k - s50.integrated) - (exp1k - exp50)) < 0.20);
  }

  // ── 4. 门限: 绝对 (-90 dBFS 段) 与相对 (-60 dBFS 段) ─────────────────────
  {
    LoudnessMeter lmA, lmB;
    lmA.SetSampleRate(kFs);
    lmB.SetSampleRate(kFs);
    ProcessSine(lmA, 5.0, -90.0, 1000.0); // 低于绝对门限: 不进入 I
    ProcessSine(lmA, 10.0, -20.0, 1000.0);
    ProcessSine(lmB, 10.0, -20.0, 1000.0); // 对照: 只有 -20 段
    LoudnessMeter::Snapshot sA, sB;
    lmA.Store(sA);
    lmB.Store(sB);
    check("4a: absolute gate excludes -90dBFS section",
          std::fabs(sA.integrated - sB.integrated) < 0.15 && sA.iValid);

    LoudnessMeter lmC, lmD;
    lmC.SetSampleRate(kFs);
    lmD.SetSampleRate(kFs);
    ProcessSine(lmC, 30.0, -10.0, 1000.0);
    ProcessSine(lmC, 20.0, -60.0, 1000.0); // 高于绝对门限, 但被相对门限剔除
    ProcessSine(lmD, 30.0, -10.0, 1000.0);
    LoudnessMeter::Snapshot sC, sD;
    lmC.Store(sC);
    lmD.Store(sD);
    check("4b: relative gate excludes -60dBFS section",
          std::fabs(sC.integrated - sD.integrated) < 0.15 && sC.iValid);
  }

  // ── 5. LRA: 双电平 ≈ 16 LU; 恒电平 ≈ 0 ───────────────────────────────────
  {
    LoudnessMeter lm;
    lm.SetSampleRate(kFs);
    for (int i = 0; i < 4; ++i) {
      ProcessSine(lm, 8.0, -10.0, 1000.0); // 32s
      ProcessSine(lm, 8.0, -26.0, 1000.0); // 32s
    }
    ProcessSine(lm, 2.0, -20.0, 1000.0); // 总计 66s >= 60s
    LoudnessMeter::Snapshot s;
    lm.Store(s);
    check("5a: two-level program LRA in [12, 19] LU",
          s.lraValid && s.range >= 12.0f && s.range <= 19.0f);

    LoudnessMeter lmFlat;
    lmFlat.SetSampleRate(kFs);
    ProcessSine(lmFlat, 70.0, -20.0, 1000.0);
    LoudnessMeter::Snapshot sf;
    lmFlat.Store(sf);
    check("5b: constant level -> LRA ~ 0", sf.lraValid && sf.range < 0.3f);
  }

  // ── 6. M/S 时间响应: 阶跃后 400ms / 3s 收敛 ──────────────────────────────
  {
    LoudnessMeter lm;
    lm.SetSampleRate(kFs);
    ProcessSine(lm, 1.0, -120.0, 1000.0); // 数字静音
    LoudnessMeter::Snapshot s;
    lm.Store(s);
    check("6a: silence -> M/S floor", s.momentary < -110.0f && s.shortTerm < -110.0f);

    const double full = ExpectedLufs(kFs, 1000.0, -20.0);
    ProcessSine(lm, 0.3, -20.0, 1000.0); // 信号开始 0.3s: M 窗 (0.9..1.3s) 含 0.1s 静音
    lm.Store(s);
    const double partial = full + 10.0 * std::log10(0.3 / 0.4); // 能量占比 3/4
    check("6b: M partially filled window (0.3s of signal)", std::fabs(s.momentary - partial) < 0.30);

    ProcessSine(lm, 0.7, -20.0, 1000.0); // 信号 1.0s: M 窗全填
    lm.Store(s);
    check("6c: M converged after 1.0s", std::fabs(s.momentary - full) < 0.20);

    ProcessSine(lm, 2.5, -20.0, 1000.0); // 信号 3.5s: S 窗全填
    lm.Store(s);
    check("6d: S converged after 3.5s", std::fabs(s.shortTerm - full) < 0.20);
  }

  // ── 7. Reset / TP 锁存 / 采样率不变性 / 不规则宿主块长 ───────────────────
  {
    LoudnessMeter lm;
    lm.SetSampleRate(kFs);
    ProcessSine(lm, 10.0, -20.0, 1000.0);
    lm.SetTruePeaks(-2.0f, -3.0f);
    lm.SetTruePeaks(-0.5f, -1.0f);
    LoudnessMeter::Snapshot s;
    lm.Store(s);
    check("7a: true peak max latch", std::fabs(s.tpMax + 0.5f) < 1e-4f);

    lm.Reset();
    lm.Store(s);
    check("7b: reset clears measurement", !s.iValid && !s.lraValid && s.tpMax < -119.0f);

    ProcessSine(lm, 5.0, -20.0, 1000.0);
    lm.Store(s);
    check("7c: recovers after reset", std::fabs(s.integrated - ExpectedLufs(kFs, 1000.0, -20.0)) < 0.20);

    // 不规则宿主块长 (3..17 循环): 引擎内部凑 100ms 子块, 结果与定长块一致
    LoudnessMeter lmOdd, lmRef;
    lmOdd.SetSampleRate(kFs);
    lmRef.SetSampleRate(kFs);
    const int n = (int)(10.0 * kFs);
    std::vector<float> L(n), R(n);
    const double amp = std::pow(10.0, -20.0 / 20.0);
    for (int i = 0; i < n; ++i) {
      const float v = (float)(amp * std::sin(2.0 * M_PI * 1000.0 * (double)i / kFs));
      L[i] = v;
      R[i] = v;
    }
    for (int pos = 0; pos < n; pos += 512)
      lmRef.Process(L.data() + pos, R.data() + pos, std::min(512, n - pos));
    for (int pos = 0, blk = 3; pos < n; pos += blk, blk = (blk % 17) + 3)
      lmOdd.Process(L.data() + pos, R.data() + pos, std::min(blk, n - pos));
    LoudnessMeter::Snapshot sOdd, sRef;
    lmOdd.Store(sOdd);
    lmRef.Store(sRef);
    check("7d: irregular host block sizes give same result",
          std::fabs(sOdd.integrated - sRef.integrated) < 0.02 &&
              std::fabs(sOdd.momentary - sRef.momentary) < 0.02 &&
              std::fabs(sOdd.shortTerm - sRef.shortTerm) < 0.02);
  }

  // ── 8. 采样率不变性 (数字域同电平 → 同 LUFS) ─────────────────────────────
  {
    double iv[3];
    const double srs[3] = {44100.0, 48000.0, 96000.0};
    for (int i = 0; i < 3; ++i) {
      LoudnessMeter lm;
      lm.SetSampleRate(srs[i]);
      ProcessSine(lm, 8.0, -20.0, 1000.0, 512, false, srs[i]);
      LoudnessMeter::Snapshot s;
      lm.Store(s);
      iv[i] = s.integrated;
      // 各自匹配本采样率下的期望 (滤波器随 fs 数字化, 允许 ±0.1)
      check(std::string("8: I matches prediction @") + std::to_string((long)srs[i]) + "Hz",
            std::fabs(s.integrated - ExpectedLufs(srs[i], 1000.0, -20.0)) < 0.10);
    }
    check("8: sample-rate invariance (spread <= 0.10 LU)",
          std::fabs(iv[0] - iv[1]) < 0.10 && std::fabs(iv[1] - iv[2]) < 0.10);
  }

  std::printf("\n%d tests, %d failures\n", tests, failures);
  return failures == 0 ? 0 : 1;
}