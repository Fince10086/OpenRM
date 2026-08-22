// dsp_test.cpp — BandPassCore 离线自测 (纯带通, 零依赖, 只验证 DSP 核心)
// 验证:
//   1) BP 模式峰值出现在中心频率
//   2) 带宽(octave) 影响通带宽度
//   3) link 模式 R 跟随 L
//   4) mix 干湿混合
//   5) 峰值增益归一化 (中心增益 ~1, 不随带宽变窄抬升)
//   6) 高 Q 稳定性
//   7) slope (12/24/48/96 dB/oct): 默认 96; -3dB 带宽跨档位保持;
//      1 个八度外衰减 ≈ 档位值; cut 前通带平直 (Butterworth HP+LP)
// 构建:  c++ -std=c++17 -O2 -o dsp_test dsp_test.cpp
#include "../plugins/BandPass/src/dsp/BandPassCore.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>

using namespace orm;

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
    printf("=== BandPassCore v0.1.1 自测 (fs=%d) ===\n", (int)kFs);

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

    // ---- 2) 带宽越窄通带越窄 (-3dB 边界 = LOWCUT/HIGHCUT) ----
    {
        auto measureBw = [](double bwOct) {
            BandPassCore core;
            core.prepare(kFs, kBlock);
            BandPassCore::Params p = MakeParams();
            p.freqL = 2000.0; p.bwL = bwOct;
            core.setParams(p);
            // 绝对 -3dB 点 (LOWCUT/HIGHCUT 的语义), 而非相对峰值
            auto edge = [&](bool low) {
                double lo = low ? 20.0 : 2000.0, hi = low ? 2000.0 : 10000.0;
                for (int it = 0; it < 50; ++it)
                {
                    const double mid = 0.5 * (lo + hi);
                    const double g = measureGain(core, mid);
                    if (low) { if (g > 0.7071) hi = mid; else lo = mid; }
                    else     { if (g > 0.7071) lo = mid; else hi = mid; }
                }
                return 0.5 * (lo + hi);
            };
            return edge(false) - edge(true);
        };
        const double bwNarrow = measureBw(0.2);
        const double bwWide   = measureBw(2.0);
        check("narrow bw (0.2 oct) < wide bw (2 oct)", bwNarrow < bwWide * 0.6);
        check("narrow bw ~ 2000*(2^0.1-2^-0.1)=277Hz", bwNarrow < 320.0 && bwNarrow > 180.0);
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
        bool wideOk = true, narrowOk = true;
        for (double bw : {0.2, 1.0, 2.0})
        {
            BandPassCore::Params p = MakeParams();
            p.freqL = 2000.0; p.bwL = bw;
            core.setParams(p);
            const double g = measureGain(core, 2000.0);
            printf("   bw=%.1f oct: center gain=%.3f\n", bw, g);
            if (bw >= 1.0) { if (std::fabs(g - 1.0) > 0.05) wideOk = false; }
            else           { if (g < 0.85) narrowOk = false; } // 窄带: 双侧过渡带重叠致中心轻微下垂
        }
        check("BP center gain ~1.0 for >=1 oct bandwidths", wideOk);
        check("BP center gain stays >=0.85 for 0.2 oct (narrow-band sag)", narrowOk);
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

    // ---- 7) slope: 默认 96; -3dB 带宽跨档位保持; 越高滚降越陡 ----
    {
        BandPassCore::Params d;
        check("slope: default is 96 dB/oct", d.slopeDbL == 96.0 && d.slopeDbR == 96.0);

        // -3dB 带宽（绝对 -3dB 点, 即 LOWCUT/HIGHCUT）不随 slope 档位改变
        auto bwAt = [](double slopeDb) {
            BandPassCore core;
            core.prepare(kFs, kBlock);
            BandPassCore::Params p = MakeParams();
            p.freqL = 1000.0; p.bwL = 1.0; p.slopeDbL = slopeDb;
            core.setParams(p);
            double lo = 20.0, hi = 1000.0;
            for (int it = 0; it < 50; ++it)
            {
                const double mid = 0.5 * (lo + hi);
                if (measureGain(core, mid) > 0.7071) hi = mid; else lo = mid;
            }
            const double lowEdge = 0.5 * (lo + hi);
            lo = 1000.0; hi = 10000.0;
            for (int it = 0; it < 50; ++it)
            {
                const double mid = 0.5 * (lo + hi);
                if (measureGain(core, mid) > 0.7071) lo = mid; else hi = mid;
            }
            const double highEdge = 0.5 * (lo + hi);
            return highEdge - lowEdge;
        };
        const double bw12 = bwAt(12.0);
        const double bw24 = bwAt(24.0);
        const double bw48 = bwAt(48.0);
        const double bw96 = bwAt(96.0);
        printf("   -3dB BW: 12=%.1f 24=%.1f 48=%.1f 96=%.1f\n", bw12, bw24, bw48, bw96);
        check("slope: -3dB BW preserved across 12/24/48/96", 
              std::fabs(bw96 - bw12) / bw12 < 0.25 &&
              std::fabs(bw48 - bw12) / bw12 < 0.25 &&
              std::fabs(bw24 - bw12) / bw12 < 0.25);

        // 高切 1414Hz (中心 1k, 1 oct 带宽): 1 个八度外 (2828Hz) 衰减应 ≈ 档位值
        auto attnDb = [](double slopeDb, double octAboveCut) {
            BandPassCore core;
            core.prepare(kFs, kBlock);
            BandPassCore::Params p = MakeParams();
            p.freqL = 1000.0; p.bwL = 1.0; p.slopeDbL = slopeDb;
            core.setParams(p);
            const double g = measureGain(core, 1414.0 * std::pow(2.0, octAboveCut));
            return 20.0 * std::log10(g + 1e-12);
        };
        const double a12 = attnDb(12.0, 1.0);
        const double a24 = attnDb(24.0, 1.0);
        const double a48 = attnDb(48.0, 1.0);
        const double a96 = attnDb(96.0, 1.0);
        printf("   1 oct 外衰减: 12=%.1f 24=%.1f 48=%.1f 96=%.1f dB\n", a12, a24, a48, a96);
        check("slope: 1 oct past cut ~ -12 dB (12)", a12 > -13.5 && a12 < -10.5);
        check("slope: 1 oct past cut ~ -24 dB (24)", a24 > -25.5 && a24 < -22.5);
        check("slope: 1 oct past cut ~ -48 dB (48)", a48 > -49.5 && a48 < -46.5);
        check("slope: 1 oct past cut ~ -96 dB (96)", a96 < -90.0);

        // cut 前通带平直: 高切内侧 0.25 oct 处衰减应 < 0.1 dB (96 档)
        BandPassCore core;
        core.prepare(kFs, kBlock);
        BandPassCore::Params p = MakeParams();
        p.freqL = 1000.0; p.bwL = 1.0; p.slopeDbL = 96.0;
        core.setParams(p);
        const double inDb = 20.0 * std::log10(measureGain(core, 1414.0 / std::pow(2.0, 0.25)) + 1e-12);
        printf("   cut 前 0.25 oct: %.3f dB\n", inDb);
        check("slope: passband flat 0.25 oct before cut (>= -0.1dB)", inDb > -0.1);
    }

    // ---- 8) 左右斜率独立 (LINK 关) / R 跟随 L (LINK 开) ----
    {
        BandPassCore core;
        core.prepare(kFs, kBlock);
        BandPassCore::Params p = MakeParams();
        p.freqL = 1000.0; p.bwL = 1.0; p.slopeDbL = 12.0;
        p.freqR = 1000.0; p.bwR = 1.0; p.slopeDbR = 96.0;
        p.linked = false;
        core.setParams(p);

        const int n = (int)(kFs * 1.0);
        std::vector<float> in(n), ol(n), orr(n);
        for (int i = 0; i < n; ++i)
            in[i] = std::sin(2.0 * M_PI * 2828.0 * i / kFs); // 高切 1414 之上 1 oct
        const int skip = (int)(kFs * 0.1);
        auto rms = [&](const std::vector<float>& v) {
            double s = 0; for (int i = skip; i < n; ++i) s += (double)v[i] * v[i];
            return std::sqrt(s / (n - skip));
        };
        const double inRms = rms(in);
        auto runDb = [&](double& l, double& r) {
            for (int pos = 0; pos < n; pos += kBlock)
            {
                const int b = std::min(kBlock, n - pos);
                core.updateSmoothing(b);
                core.process(in.data() + pos, in.data() + pos, ol.data() + pos, orr.data() + pos, b);
            }
            l = 20.0 * std::log10(rms(ol) / inRms + 1e-12);
            r = 20.0 * std::log10(rms(orr) / inRms + 1e-12);
        };

        double l1, r1;
        runDb(l1, r1);
        printf("   unlinked: L=%.1f dB  R=%.1f dB\n", l1, r1);
        check("slope unlinked: L(-12) far weaker than R(-96)", l1 > -14.0 && r1 < -90.0);

        p.linked = true; p.slopeDbR = 96.0; // R 参数仍为 96, LINK 时核心应强制跟随 L
        core.setParams(p);
        double l2, r2;
        runDb(l2, r2);
        printf("   linked:   L=%.1f dB  R=%.1f dB\n", l2, r2);
        check("slope linked: R follows L (~-12 both)", std::fabs(r2 - l2) < 1.0 && r2 > -14.0);
    }

    printf("=== %s (%d failures) ===\n", failures == 0 ? "ALL PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
