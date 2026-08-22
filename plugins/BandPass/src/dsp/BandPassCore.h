#pragma once

#include <array>
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

// One channel of the band-pass: two independent Butterworth filters in series,
// a high-pass at LOWCUT followed by a low-pass at HIGHCUT. Each edge has
// M = 2*(slopeDb/12) poles (12/24/48/96 dB/oct -> 2/4/8/16 poles), realized as
// M/2 cascaded 2nd-order TPT SVF sections whose Q values come from the
// Butterworth pole angles: Q_k = 1/(2*cos(theta_k)), theta_k = (2k-1)*pi/(2M).
// This is a true Butterworth response (unlike an equal-Q cascade): maximally
// flat passband, -3 dB exactly at the cut, and 6M dB/oct rolloff — one octave
// past the cut attenuates by the selected slope value, independent of bandwidth.
class ChannelChain
{
public:
    static constexpr int kMaxSections = 8; // 96 dB/oct per edge = 16 poles = 8 sections

    void prepare(double sr) noexcept
    {
        mSampleRate = sr;
        for (auto& f : mHp) f.prepare(sr);
        for (auto& f : mLp) f.prepare(sr);
        reset();
    }
    void reset() noexcept
    {
        for (auto& f : mHp) f.reset();
        for (auto& f : mLp) f.reset();
    }

    // lowHz: LOWCUT (high-pass cutoff), highHz: HIGHCUT (low-pass cutoff),
    // slopeDb: rolloff in dB/oct (12/24/48/96 -> 1/2/4/8 sections per edge).
    void setParams(double lowHz, double highHz, double slopeDb) noexcept
    {
        const int sections = std::clamp((int) std::lround(slopeDb / 12.0), 1, kMaxSections);
        if (sections != mSections)
        {
            mSections = sections;
            for (auto& f : mHp) f.reset();
            for (auto& f : mLp) f.reset();
        }
        const int poles = 2 * sections;
        for (int k = 0; k < sections; ++k)
        {
            const double theta = (2.0 * k + 1.0) * M_PI / (2.0 * poles);
            mQ[k] = 1.0 / (2.0 * std::cos(theta));
            mHp[k].setParams(lowHz, mQ[k]);
            mLp[k].setParams(highHz, mQ[k]);
        }
    }

    // Agitation: per-sample cutoff update, keeping the per-section Q values.
    void setFreqs(double lowHz, double highHz) noexcept
    {
        for (int k = 0; k < mSections; ++k)
        {
            mHp[k].setParams(lowHz, mQ[k]);
            mLp[k].setParams(highHz, mQ[k]);
        }
    }

    inline float process(float x) noexcept
    {
        for (int k = 0; k < mSections; ++k)
        {
            float lo, b, h;
            mHp[k].process(x, lo, b, h);
            x = h;
        }
        for (int k = 0; k < mSections; ++k)
        {
            float lo, b, h;
            mLp[k].process(x, lo, b, h);
            x = lo;
        }
        return x;
    }

private:
    double mSampleRate = 44100.0;
    int    mSections = 1;
    double mQ[kMaxSections] = {};
    std::array<SvfFilter, kMaxSections> mHp;
    std::array<SvfFilter, kMaxSections> mLp;
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
        double slopeDbL = 96.0; // L rolloff in dB/oct: 12/24/48/96
        double slopeDbR = 96.0; // R rolloff in dB/oct: 12/24/48/96
    };

    BandPassCore() = default;

    void prepare(double sampleRate, int ) noexcept
    {
        mSampleRate = sampleRate;
        mChainL.prepare(sampleRate);
        mChainR.prepare(sampleRate);
        mSmFreqL = mParams.freqL; mSmBwL = mParams.bwL;
        mSmFreqR = mParams.freqR; mSmBwR = mParams.bwR;
        mHalfBwL = std::pow(2.0, mSmBwL * 0.5);
        mHalfBwR = std::pow(2.0, mSmBwR * 0.5);
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
        mSlopeDbL = p.slopeDbL;
        mSlopeDbR = p.slopeDbR;
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

        mHalfBwL = std::pow(2.0, mSmBwL * 0.5);
        mHalfBwR = std::pow(2.0, mSmBwR * 0.5);
        mChainL.setParams(mSmFreqL / mHalfBwL, mSmFreqL * mHalfBwL, mSlopeDbL);
        mChainR.setParams(mSmFreqR / mHalfBwR, mSmFreqR * mHalfBwR,
                          mParams.linked ? mSlopeDbL : mSlopeDbR);
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
                mChainL.setFreqs(fL / mHalfBwL, fL * mHalfBwL);
                mChainR.setFreqs(fR / mHalfBwR, fR * mHalfBwR);
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
                mChainL.setFreqs(f / mHalfBwL, f * mHalfBwL);
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
    double mSlopeDbL = 96.0, mSlopeDbR = 96.0;

    double mSmFreqL = 1000.0, mSmBwL = 1.0;
    double mSmFreqR = 1000.0, mSmBwR = 1.0;
    double mHalfBwL = 1.0, mHalfBwR = 1.0;
};

}
