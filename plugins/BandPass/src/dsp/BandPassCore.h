// ============================================================================
// BandPassCore.h — 零依赖立体声带通滤波器核心 (GRM Tools BandPass 风格 v0.2)
//
// 算法: TPT State Variable Filter (Zavalishin / Cytomic)
//   每通道信号链: HP(slider) -> BP(center/bw, 小球控制) -> LP(slider) -> gain
//   pass 模式: 0=BP 1=HP 2=LP (选择 BP 段的输出形态)
//   bandwidth: octave 带宽, 内部转 Q
// 特性:
//   - float32 处理路径, double 系数计算 (每 block 一次)
//   - 双声道独立 (dual-mono) / link 模式
//   - 一阶参数平滑防 zipper
//   - agitation: 正弦 LFO 对中心频率调制 (强度/速率)
//   - pan 路由: L->R / R->L / Flip
//   - mix 干湿混合
//   - 零依赖纯头文件, C++17
// ============================================================================
#pragma once

#include <cmath>
#include <algorithm>

namespace grm {

// ----------------------------------------------------------------------------
// 单声道 TPT SVF: 一次计算 low / band / high 三个输出
// ----------------------------------------------------------------------------
class SvfFilter
{
public:
    void prepare(double sampleRate) noexcept { mSampleRate = sampleRate; reset(); }
    void reset() noexcept { mIc1eq = 0.0f; mIc2eq = 0.0f; }

    void setParams(double freqHz, double q) noexcept
    {
        const double f = std::clamp(freqHz, 20.0, mSampleRate * 0.49);
        const double k = 1.0 / std::clamp(q, 0.1, 40.0);
        const double g = std::tan(M_PI * f / mSampleRate);
        mA1 = 1.0 / (1.0 + g * (g + k));
        mA2 = g * mA1;
        mA3 = g * g * mA1;
        mK = k;
    }

    inline void process(float x, float& low, float& band, float& high) noexcept
    {
        const float v3 = x - mIc2eq;
        const float v1 = static_cast<float>(mA1) * mIc1eq + static_cast<float>(mA2) * v3;
        const float v2 = mIc2eq + static_cast<float>(mA2) * mIc1eq + static_cast<float>(mA3) * v3;
        mIc1eq = 2.0f * v1 - mIc1eq;
        mIc2eq = 2.0f * v2 - mIc2eq;
        low  = v2;
        band = v1;
        high = x - static_cast<float>(mK) * v1 - v2;
    }

private:
    double mSampleRate = 44100.0;
    float  mIc1eq = 0.0f;
    float  mIc2eq = 0.0f;
    double mA1 = 0.0, mA2 = 0.0, mA3 = 0.0, mK = 0.0;
};

// octave 带宽 -> Q  (BW=1 oct => Q~1.7, BW=0.1 => Q~14)
inline double octaveToQ(double oct) noexcept
{
    const double w = std::pow(2.0, oct * 0.5) - std::pow(2.0, -oct * 0.5);
    return 1.0 / std::max(w, 1e-3);
}

// ----------------------------------------------------------------------------
// 单通道滤波链: HP -> BP -> LP, 含 pass 模式选择与增益
// ----------------------------------------------------------------------------
class ChannelChain
{
public:
    void prepare(double sr) noexcept
    {
        mSampleRate = sr;
        hp.prepare(sr); bp.prepare(sr); lp.prepare(sr);
        reset();
    }
    void reset() noexcept { hp.reset(); bp.reset(); lp.reset(); }

    void setParams(double hpHz, double bpHz, double bwOct, int passMode, double lpHz) noexcept
    {
        hp.setParams(hpHz, 0.707);
        bp.setParams(bpHz, octaveToQ(bwOct));
        lp.setParams(lpHz, 0.707);
        mPassMode = passMode;
    }

    inline float process(float x) noexcept
    {
        // HP 段: 取 high 输出 (高通)
        float lo1, b1, h1;
        hp.process(x, lo1, b1, h1);
        // BP 段: pass 模式选择输出形态 (band / high / low)
        float lo2, b2, h2;
        bp.process(h1, lo2, b2, h2);
        float stage = b2;
        if (mPassMode == 1) stage = h2;        // PASS: 高通
        else if (mPassMode == 2) stage = lo2;  // PASS: 低通
        // LP 段: 取 low 输出 (低通)
        float lo3, b3, h3;
        lp.process(stage, lo3, b3, h3);
        return lo3;
    }

    SvfFilter hp, bp, lp;

private:
    double mSampleRate = 44100.0;
    int mPassMode = 0;
};

// ----------------------------------------------------------------------------
// 立体声 BandPass 核心
// ----------------------------------------------------------------------------
class BandPassCore
{
public:
    struct Params
    {
        // 每通道
        double freqL = 1000.0;   // 中心频率 Hz 20..20000
        double bwL   = 1.0;      // 带宽 octave 0.05..4
        double hpL   = 20.0;     // 高通截止 Hz
        double lpL   = 20000.0;  // 低通截止 Hz
        int    passL = 0;        // 0=BP 1=HP 2=LP
        float  gainL = 1.0f;

        double freqR = 1000.0;
        double bwR   = 1.0;
        double hpR   = 20.0;
        double lpR   = 20000.0;
        int    passR = 0;
        float  gainR = 1.0f;

        // 全局
        bool   linked   = false;
        float  mix      = 1.0f;   // 干湿 0..1
        bool   agOn     = false;  // agitation
        float  agAmount = 0.1f;   // 抖动强度 0..1
        double agRate   = 1.0;    // 抖动速率 Hz 0.05..20
        bool   panLR    = false;  // L 输出混入 R 输入
        bool   panRL    = false;  // R 输出混入 L 输入
        bool   panFlip  = false;  // 交换输入
    };

    BandPassCore() = default;

    void prepare(double sampleRate, int /*maxBlock*/) noexcept
    {
        mSampleRate = sampleRate;
        mChainL.prepare(sampleRate);
        mChainR.prepare(sampleRate);
        // 平滑状态快照
        mSmFreqL = mParams.freqL; mSmBwL = mParams.bwL;
        mSmHpL = mParams.hpL;     mSmLpL = mParams.lpL;
        mSmFreqR = mParams.freqR; mSmBwR = mParams.bwR;
        mSmHpR = mParams.hpR;     mSmLpR = mParams.lpR;
        mAgPhase = 0.0;
        reset();
    }

    void reset() noexcept { mChainL.reset(); mChainR.reset(); }

    void setParams(const Params& p) noexcept
    {
        mParams = p;
        mParams.gainL = std::clamp(p.gainL, 0.0f, 2.0f);
        mParams.gainR = std::clamp(p.gainR, 0.0f, 2.0f);
        mParams.mix   = std::clamp(p.mix, 0.0f, 1.0f);
        mParams.agAmount = std::clamp(p.agAmount, 0.0f, 1.0f);
    }

    const Params& getParams() const noexcept { return mParams; }

    void updateSmoothing(int blockSize) noexcept
    {
        const double coef = 1.0 - std::exp(-1.0 / (0.015 * mSampleRate / (double)blockSize + 1.0));
        mSmFreqL += (mParams.freqL - mSmFreqL) * coef;
        mSmBwL   += (mParams.bwL   - mSmBwL)   * coef;
        mSmHpL   += (mParams.hpL   - mSmHpL)   * coef;
        mSmLpL   += (mParams.lpL   - mSmLpL)   * coef;

        if (mParams.linked)
        {
            mSmFreqR = mSmFreqL; mSmBwR = mSmBwL;
            mSmHpR   = mSmHpL;   mSmLpR = mSmLpL;
        }
        else
        {
            mSmFreqR += (mParams.freqR - mSmFreqR) * coef;
            mSmBwR   += (mParams.bwR   - mSmBwR)   * coef;
            mSmHpR   += (mParams.hpR   - mSmHpR)   * coef;
            mSmLpR   += (mParams.lpR   - mSmLpR)   * coef;
        }

        mChainL.setParams(mSmHpL, mSmFreqL, mSmBwL, mParams.passL, mSmLpL);
        mChainR.setParams(mSmHpR, mSmFreqR, mSmBwR, mParams.linked ? mParams.passL : mParams.passR, mSmLpR);
    }

    // 立体声处理 (支持每声道原位 in==out)
    void process(const float* const inL, const float* const inR,
                 float* const outL, float* const outR, int n) noexcept
    {
        const float gL = mParams.gainL, gR = mParams.gainR;
        const float mix = mParams.mix;
        const bool flip = mParams.panFlip;
        const bool panLR = mParams.panLR, panRL = mParams.panRL;
        const float agAmt = mParams.agOn ? mParams.agAmount : 0.0f;
        const double agInc = 2.0 * M_PI * mParams.agRate / mSampleRate;

        for (int i = 0; i < n; ++i)
        {
            // agitation: 正弦 LFO 调制中心频率
            double mod = 1.0;
            if (agAmt > 0.0f)
            {
                mAgPhase += agInc;
                if (mAgPhase > 2.0 * M_PI) mAgPhase -= 2.0 * M_PI;
                mod = 1.0 + agAmt * std::sin(mAgPhase);
            }

            const float xl = inL[i], xr = inR[i];
            const float a = flip ? xr : xl;
            const float b = flip ? xl : xr;

            float wetL, wetR;
            if (mod != 1.0)
            {
                // 抖动时按样本重算 BP 系数
                const double fL = std::clamp(mSmFreqL * mod, 20.0, mSampleRate * 0.49);
                const double fR = std::clamp(mSmFreqR * mod, 20.0, mSampleRate * 0.49);
                mChainL.bp.setParams(fL, octaveToQ(mSmBwL));
                mChainR.bp.setParams(fR, octaveToQ(mSmBwR));
            }
            wetL = mChainL.process(a) * gL;
            wetR = mChainR.process(b) * gR;

            // 声像路由
            float outWetL = wetL, outWetR = wetR;
            if (panLR) outWetL = wetL + wetR * 0.5f;
            if (panRL) outWetR = wetR + wetL * 0.5f;

            // 干湿混合
            outL[i] = a * (1.0f - mix) + outWetL * mix;
            outR[i] = b * (1.0f - mix) + outWetR * mix;
        }
    }

    // 单声道
    void process(const float* const in, float* const out, int n) noexcept
    {
        const float g = mParams.gainL;
        const float mix = mParams.mix;
        const float agAmt = mParams.agOn ? mParams.agAmount : 0.0f;
        const double agInc = 2.0 * M_PI * mParams.agRate / mSampleRate;
        for (int i = 0; i < n; ++i)
        {
            double mod = 1.0;
            if (agAmt > 0.0f)
            {
                mAgPhase += agInc;
                if (mAgPhase > 2.0 * M_PI) mAgPhase -= 2.0 * M_PI;
                mod = 1.0 + agAmt * std::sin(mAgPhase);
            }
            if (mod != 1.0)
            {
                const double f = std::clamp(mSmFreqL * mod, 20.0, mSampleRate * 0.49);
                mChainL.bp.setParams(f, octaveToQ(mSmBwL));
            }
            const float wet = mChainL.process(in[i]) * g;
            out[i] = in[i] * (1.0f - mix) + wet * mix;
        }
    }

private:
    double mSampleRate = 44100.0;
    Params mParams;
    ChannelChain mChainL, mChainR;
    double mAgPhase = 0.0;

    // 平滑后的参数状态
    double mSmFreqL = 1000.0, mSmBwL = 1.0, mSmHpL = 20.0, mSmLpL = 20000.0;
    double mSmFreqR = 1000.0, mSmBwR = 1.0, mSmHpR = 20.0, mSmLpR = 20000.0;
};

} // namespace grm
