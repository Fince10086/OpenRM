// dsp_test.cpp — BandPassCore 离线自测 (纯带通, 零依赖, 只验证 DSP 核心)
// 验证:
//   1) BP 模式峰值出现在中心频率
//   2) 带宽(octave) 影响通带宽度
//   3) link 模式 R 跟随 L
//   4) mix 干湿混合
//   5) 峰值增益归一化 (中心增益 ~1, 不随带宽变窄抬升)
//   6) 高 Q 稳定性
// 构建:  c++ -std=c++17 -O2 -o dsp_test dsp_test.cpp
#include "../plugins/BandPass/src/dsp/BandPassCore.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>

using namespace grm;

static constexpr double kFs = 48000.0;
static constexpr int    kBlock = 512;

static double measureGain(BandPassCore& core, double freqHz, double seconds = 1.0)
{
    const int n = (int)(kFs * seconds);
    std::vector<float> in(n), out(n);
    for (int i = 0; i < n; ++i)
        in[i] = std::sin(2.0 * M_PI * freqHz * i / kFs);

    for (int pos = 0; pos < n; pos += kBlock)
    {
        const int b = std::min(kBlock, n - pos);
        core.updateSmoothing(b);
        core.process(in.data() + pos, out.data() + pos, b);
    }

    double inRms = 0.0, outRms = 0.0;
    const int skip = (int)(kFs * 0.1);
    for (int i = skip; i < n; ++i)
    {
        inRms += (double)in[i] * in[i];
        outRms += (double)out[i] * out[i];
    }
    const int m = n - skip;
    return std::sqrt(outRms / m) / (std::sqrt(inRms / m) + 1e-12);
}

static BandPassCore::Params MakeParams()
{
    BandPassCore::Params p;
    p.freqL = 1000.0; p.bwL = 1.0; p.gainL = 1.0f;
    p.freqR = 1000.0; p.bwR = 1.0; p.gainR = 1.0f;
    p.mix = 1.0f;
    return p;
}

static int failures = 0;
static void check(const std::string& name, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++failures;
}

int main()
{
    printf("=== BandPassCore v0.0.1 自测 (fs=%d) ===\n", (int)kFs);

    // ---- 1) BP 峰值在中心频率 ----
    {
        BandPassCore core;
        core.prepare(kFs, kBlock);
        BandPassCore::Params p = MakeParams();
        p.freqL = 2000.0; p.bwL = 1.0;
        core.setParams(p);
        const double gC = measureGain(core, 2000.0);
        const double gH = measureGain(core, 1000.0);
        const double gD = measureGain(core, 4000.0);
        check("BP: peak at center 2k", gC >= gH && gC >= gD);
        check("BP: attenuation at f0/2", gH < gC * 0.5);
        printf("   gains: 1k=%.3f 2k=%.3f 4k=%.3f\n", gH, gC, gD);
    }

    // ---- 2) 带宽越窄通带越窄 ----
    {
        auto measureBw = [](double bwOct) {
            BandPassCore core;
            core.prepare(kFs, kBlock);
            BandPassCore::Params p = MakeParams();
            p.freqL = 2000.0; p.bwL = bwOct;
            core.setParams(p);
            const double gPeak = measureGain(core, 2000.0);
            auto edge = [&](bool low) {
                double lo = low ? 20.0 : 2000.0, hi = low ? 2000.0 : 10000.0;
                for (int it = 0; it < 50; ++it)
                {
                    const double mid = 0.5 * (lo + hi);
                    const double g = measureGain(core, mid);
                    if (low) { if (g > gPeak * 0.7071) hi = mid; else lo = mid; }
                    else     { if (g > gPeak * 0.7071) lo = mid; else hi = mid; }
                }
                return 0.5 * (lo + hi);
            };
            return edge(false) - edge(true);
        };
        const double bwNarrow = measureBw(0.2);
        const double bwWide   = measureBw(2.0);
        check("narrow bw (0.2 oct) < wide bw (2 oct)", bwNarrow < bwWide * 0.6);
        check("narrow bw ~ f0/12 (2000/12~166)", bwNarrow < 300.0 && bwNarrow > 80.0);
        printf("   -3dB BW: 0.2oct=%.1f Hz, 2oct=%.1f Hz\n", bwNarrow, bwWide);
    }

    // ---- 3) link: R 跟随 L ----
    {
        BandPassCore core;
        core.prepare(kFs, kBlock);
        BandPassCore::Params p = MakeParams();
        p.freqL = 3000.0; p.bwL = 0.5;
        p.freqR = 500.0;  p.bwR = 0.5;
        p.linked = true;
        core.setParams(p);
        const int n = (int)(kFs * 0.5);
        std::vector<float> in(n), ol(n), orr(n);
        for (int i = 0; i < n; ++i)
            in[i] = std::sin(2.0 * M_PI * 3000.0 * i / kFs) * 0.5f +
                    std::sin(2.0 * M_PI * 500.0 * i / kFs) * 0.5f;
        for (int pos = 0; pos < n; pos += kBlock)
        {
            const int b = std::min(kBlock, n - pos);
            core.updateSmoothing(b);
            core.process(in.data() + pos, in.data() + pos, ol.data() + pos, orr.data() + pos, b);
        }
        const int skip = (int)(kFs * 0.1);
        auto rms = [&](const std::vector<float>& v) {
            double s = 0; for (int i = skip; i < n; ++i) s += (double)v[i] * v[i];
            return std::sqrt(s / (n - skip));
        };
        const double rL = rms(ol), rR = rms(orr);
        check("linked: R follows L", std::fabs(rR - rL) / (rL + 1e-9) < 0.05);
        printf("   linked RMS: L=%.4f R=%.4f\n", rL, rR);
    }

    // ---- 4) mix 干湿 ----
    {
        BandPassCore core;
        core.prepare(kFs, kBlock);
        BandPassCore::Params p = MakeParams();
        p.freqL = 1000.0; p.bwL = 1.0; p.mix = 0.0f; // 全干: 输出=输入
        core.setParams(p);
        const double gDry = measureGain(core, 2000.0);
        p.mix = 1.0f;
        core.setParams(p);
        const double gWet = measureGain(core, 2000.0);
        check("mix=0 -> dry passthrough (gain~1)", std::fabs(gDry - 1.0) < 0.01);
        check("mix=1 -> wet only (filtered)", gWet < 0.9);
        printf("   mix: dry=%.3f wet=%.3f\n", gDry, gWet);
    }

    // ---- 5) 峰值增益归一化: 不同带宽下中心频率增益 ~1 ----
    {
        BandPassCore core;
        core.prepare(kFs, kBlock);
        bool normOk = true;
        for (double bw : {0.2, 1.0, 2.0})
        {
            BandPassCore::Params p = MakeParams();
            p.freqL = 2000.0; p.bwL = bw;
            core.setParams(p);
            const double g = measureGain(core, 2000.0);
            printf("   bw=%.1f oct: center gain=%.3f\n", bw, g);
            if (std::fabs(g - 1.0) > 0.05) normOk = false;
        }
        check("BP peak gain normalized (~1.0) across bandwidths", normOk);
    }

    // ---- 6) 稳定性: 白噪声高 Q ----
    {
        BandPassCore core;
        core.prepare(kFs, kBlock);
        BandPassCore::Params p = MakeParams();
        p.freqL = 4000.0; p.bwL = 0.05; p.freqR = 250.0; p.bwR = 0.1;
        p.agOn = true; p.agAmount = 0.5f; p.agRate = 8.0;
        core.setParams(p);
        const int n = (int)(kFs * 10);
        std::vector<float> in(n), out(n);
        unsigned s = 12345;
        for (int i = 0; i < n; ++i) { s = s * 1664525u + 1013904223u; in[i] = (float)(s / 4294967296.0 * 2.0 - 1.0); }
        for (int pos = 0; pos < n; pos += kBlock)
        {
            const int b = std::min(kBlock, n - pos);
            core.updateSmoothing(b);
            core.process(in.data() + pos, in.data() + pos, out.data() + pos, out.data() + pos, b);
        }
        bool stable = true;
        for (int i = 0; i < n; ++i)
            if (!std::isfinite(out[i]) || std::fabs(out[i]) > 16.0f) { stable = false; break; }
        check("stable under narrow-band noise + agitation (10s, |out|<16)", stable);
    }

    printf("=== %s (%d failures) ===\n", failures == 0 ? "ALL PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
