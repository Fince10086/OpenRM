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

class ChannelChain
{
public:
    static constexpr int kMaxSections = 8;
    void prepare(double sr) noexcept
    {
        for (auto& f : mHp) f.prepare(sr);
        for (auto& f : mLp) f.prepare(sr);
    }
    void reset() noexcept
    {
        for (auto& f : mHp) f.reset();
        for (auto& f : mLp) f.reset();
    }

    void update(double lowHz, double highHz, double slopeDb) noexcept
    {
        const int sections = std::clamp((int) std::lround(slopeDb / 12.0), 1, kMaxSections);
        if (sections != mSections)
        {
            mSections = sections;
            const int poles = 2 * sections;
            for (int k = 0; k < sections; ++k)
            {
                mQ[k] = 1.0 / (2.0 * std::cos((2.0 * k + 1.0) * M_PI / (2.0 * poles)));
                mHp[k].reset();
                mLp[k].reset();
            }
        }
        for (int k = 0; k < sections; ++k)
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
    int    mSections = 0;
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
        double slopeDbL = 96.0;
        double slopeDbR = 96.0;
    };

    BandPassCore() = default;

    void prepare(double sampleRate) noexcept
    {
        mSampleRate = sampleRate;
        for (auto& ch : mCh) ch.prepare(sampleRate);
        mAgPhase = 0.0;
        reset();
    }
    void reset() noexcept
    {
        for (auto& ch : mCh) ch.reset();
    }
    void setParams(const Params& p) noexcept
    {
        mParams = p;
        mParams.gainL = std::clamp(p.gainL, 0.0f, 2.0f);
        mParams.gainR = std::clamp(p.gainR, 0.0f, 2.0f);
        mParams.mix   = std::clamp(p.mix, 0.0f, 1.0f);
        mParams.agAmount = std::clamp(p.agAmount, 0.0f, 1.0f);
        if (p.linked)
        {
            mParams.freqR    = mParams.freqL;
            mParams.bwR      = mParams.bwL;
            mParams.slopeDbR = mParams.slopeDbL;
        }
        mCh[0].setTarget(mParams.freqL, mParams.bwL, mParams.slopeDbL);
        mCh[1].setTarget(mParams.freqR, mParams.bwR, mParams.slopeDbR);
    }
    void updateSmoothing(int blockSize) noexcept
    {
        const double coef = 1.0 - std::exp(-(double)blockSize / (0.015 * mSampleRate));
        for (auto& ch : mCh) ch.smooth(coef);
    }
    void process(const float* const inL, const float* const inR,
                 float* const outL, float* const outR, int n,
                 float* wetOutL = nullptr, float* wetOutR = nullptr) noexcept
    {
        const float* in[2]  = { inL, inR };
        float* out[2] = { outL, outR };
        float* wet[2] = { wetOutL, wetOutR };
        run(in, out, wet, 2, n);
    }
    void process(const float* const in, float* const out, int n,
                 float* wetOut = nullptr) noexcept
    {
        const float* ins[1] = { in };
        float* outs[1] = { out };
        float* wets[1] = { wetOut };
        run(ins, outs, wets, 1, n);
    }

private:
    static constexpr double kAgitationMaxOct = 0.5;

    struct Channel
    {
        void prepare(double sr) noexcept
        {
            chain.prepare(sr);
            smFreq = targetFreq;
            smBw = targetBw;
            halfBw = std::pow(2.0, smBw * 0.5);
        }
        void reset() noexcept { chain.reset(); }
        void setTarget(double freq, double bw, double slope) noexcept
        {
            targetFreq = freq;
            targetBw = bw;
            slopeDb = slope;
        }
        
        void smooth(double coef) noexcept
        {
            smFreq += (targetFreq - smFreq) * coef;
            smBw   += (targetBw - smBw) * coef;
            halfBw = std::pow(2.0, smBw * 0.5);
            chain.update(smFreq / halfBw, smFreq * halfBw, slopeDb);
        }
        inline float process(float x, double modOct, double sampleRate) noexcept
        {
            if (modOct != 0.0)
            {
                const double f = std::clamp(smFreq * std::exp2(modOct), 20.0, sampleRate * 0.49);
                chain.update(f / halfBw, f * halfBw, slopeDb);
            }
            return chain.process(x);
        }
        double targetFreq = 1000.0, targetBw = 1.0;
        double smFreq = 1000.0, smBw = 1.0, halfBw = 1.0;
        double slopeDb = 96.0;
        ChannelChain chain;
    };

    void run(const float* const* in, float* const* out, float* const* wet,
             int numCh, int n) noexcept
    {
        const float gains[2] = { mParams.gainL, mParams.gainR };
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
            for (int c = 0; c < numCh; ++c)
            {
                const float x = in[c][i];
                const float wetS = mCh[c].process(x, modOct, mSampleRate) * gains[c];
                if (wet[c]) wet[c][i] = wetS;
                out[c][i] = x * dryGain + wetS * wetGain;
            }
        }
    }

    double mSampleRate = 44100.0;
    Params mParams;
    Channel mCh[2];
    double mAgPhase = 0.0;
};

}
