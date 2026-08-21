#pragma once

#include <cmath>
#include <algorithm>
#include <atomic>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace orm {

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

        if (std::fabs(mIc1eq) < 1e-20f) mIc1eq = 0.f;
        if (std::fabs(mIc2eq) < 1e-20f) mIc2eq = 0.f;
    }

private:
    double mSampleRate = 44100.0;
    float  mIc1eq = 0.0f;
    float  mIc2eq = 0.0f;
    double mA1 = 0.0, mA2 = 0.0, mA3 = 0.0, mK = 0.0;
};

inline double octaveToQ(double oct) noexcept
{
    const double w = std::pow(2.0, oct * 0.5) - std::pow(2.0, -oct * 0.5);
    return 1.0 / std::max(w, 1e-3);
}

template <typename T>
class ParamMailbox
{
public:
    void publish(const T& p) noexcept
    {
        const unsigned v = mVersion.load(std::memory_order_relaxed);
        mVersion.store(v + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        mSnapshot = p;
        mVersion.store(v + 2, std::memory_order_release);
    }

    bool consume(T& out) noexcept
    {
        const unsigned v1 = mVersion.load(std::memory_order_acquire);
        if (v1 & 1u) return false;
        out = mSnapshot;
        std::atomic_thread_fence(std::memory_order_acquire);
        return v1 == mVersion.load(std::memory_order_relaxed);
    }

private:
    std::atomic<unsigned> mVersion {0};
    T mSnapshot {};
};

class ChannelChain
{
public:
    void prepare(double sr) noexcept
    {
        mSampleRate = sr;
        bp.prepare(sr);
        reset();
    }
    void reset() noexcept { bp.reset(); }

    void setParams(double bpHz, double bwOct) noexcept
    {
        mQ = octaveToQ(bwOct);
        mBandNorm = 1.0 / mQ;
        bp.setParams(bpHz, mQ);
    }

    void setFreq(double bpHz) noexcept { bp.setParams(bpHz, mQ); }

    inline float process(float x) noexcept
    {
        float lo, b, h;
        bp.process(x, lo, b, h);
        return b * static_cast<float>(mBandNorm);
    }

    SvfFilter bp;

private:
    double mSampleRate = 44100.0;
    double mQ = 1.0;
    double mBandNorm = 1.0;
};

class BandPassCore
{
public:
    struct Params
    {
        double freqL = 1000.0;
        double bwL   = 1.0;
        float  gainL = 1.0f;

        double freqR = 1000.0;
        double bwR   = 1.0;
        float  gainR = 1.0f;

        bool   linked   = false;
        float  mix      = 1.0f;
        bool   agOn     = false;
        float  agAmount = 0.1f;
        double agRate   = 1.0;
    };

    BandPassCore() = default;

    void prepare(double sampleRate, int ) noexcept
    {
        mSampleRate = sampleRate;
        mChainL.prepare(sampleRate);
        mChainR.prepare(sampleRate);
        mSmFreqL = mParams.freqL; mSmBwL = mParams.bwL;
        mSmFreqR = mParams.freqR; mSmBwR = mParams.bwR;
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
        const double coef = 1.0 - std::exp(-(double)blockSize / (0.015 * mSampleRate));
        mSmFreqL += (mParams.freqL - mSmFreqL) * coef;
        mSmBwL   += (mParams.bwL   - mSmBwL)   * coef;

        if (mParams.linked)
        {
            mSmFreqR += (mParams.freqL - mSmFreqR) * coef;
            mSmBwR   += (mParams.bwL   - mSmBwR)   * coef;
        }
        else
        {
            mSmFreqR += (mParams.freqR - mSmFreqR) * coef;
            mSmBwR   += (mParams.bwR   - mSmBwR)   * coef;
        }

        mChainL.setParams(mSmFreqL, mSmBwL);
        mChainR.setParams(mSmFreqR, mSmBwR);
    }

    void process(const float* const inL, const float* const inR,
                 float* const outL, float* const outR, int n) noexcept
    {
        const float gL = mParams.gainL, gR = mParams.gainR;
        const float mixAngle = mParams.mix * 0.5f * static_cast<float>(M_PI);
        const float dryGain = std::cos(mixAngle), wetGain = std::sin(mixAngle);
        const float agAmt = mParams.agOn ? mParams.agAmount : 0.0f;
        const double agInc = 2.0 * M_PI * mParams.agRate / mSampleRate;

        for (int i = 0; i < n; ++i)
        {
            double modOct = 0.0;
            if (agAmt > 0.0f)
            {
                mAgPhase += agInc;
                if (mAgPhase > 2.0 * M_PI) mAgPhase -= 2.0 * M_PI;
                modOct = agAmt * kAgitationMaxOct * std::sin(mAgPhase);
            }

            const float xl = inL[i], xr = inR[i];

            float wetL, wetR;
            if (modOct != 0.0)
            {
                const double fL = std::clamp(mSmFreqL * std::exp2(modOct), 20.0, mSampleRate * 0.49);
                const double fR = std::clamp(mSmFreqR * std::exp2(modOct), 20.0, mSampleRate * 0.49);
                mChainL.setFreq(fL);
                mChainR.setFreq(fR);
            }
            wetL = mChainL.process(xl) * gL;
            wetR = mChainR.process(xr) * gR;

            outL[i] = xl * dryGain + wetL * wetGain;
            outR[i] = xr * dryGain + wetR * wetGain;
        }
    }

    void process(const float* const in, float* const out, int n) noexcept
    {
        const float g = mParams.gainL;
        const float mixAngle = mParams.mix * 0.5f * static_cast<float>(M_PI);
        const float dryGain = std::cos(mixAngle), wetGain = std::sin(mixAngle);
        const float agAmt = mParams.agOn ? mParams.agAmount : 0.0f;
        const double agInc = 2.0 * M_PI * mParams.agRate / mSampleRate;
        for (int i = 0; i < n; ++i)
        {
            double modOct = 0.0;
            if (agAmt > 0.0f)
            {
                mAgPhase += agInc;
                if (mAgPhase > 2.0 * M_PI) mAgPhase -= 2.0 * M_PI;
                modOct = agAmt * kAgitationMaxOct * std::sin(mAgPhase);
            }
            if (modOct != 0.0)
            {
                const double f = std::clamp(mSmFreqL * std::exp2(modOct), 20.0, mSampleRate * 0.49);
                mChainL.setFreq(f);
            }
            const float wet = mChainL.process(in[i]) * g;
            out[i] = in[i] * dryGain + wet * wetGain;
        }
    }

private:
    static constexpr double kAgitationMaxOct = 0.5;

    double mSampleRate = 44100.0;
    Params mParams;
    ChannelChain mChainL, mChainR;
    double mAgPhase = 0.0;

    double mSmFreqL = 1000.0, mSmBwL = 1.0;
    double mSmFreqR = 1000.0, mSmBwR = 1.0;
};

}
