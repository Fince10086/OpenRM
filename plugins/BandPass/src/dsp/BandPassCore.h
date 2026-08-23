#pragma once

#include <array>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstdint>

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

    void update(double lowHz, double highHz, double slopeDb, bool reject) noexcept
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
        const double hpFreq = reject ? highHz : lowHz;
        const double lpFreq = reject ? lowHz   : highHz;
        for (int k = 0; k < sections; ++k)
        {
            mHp[k].setParams(hpFreq, mQ[k]);
            mLp[k].setParams(lpFreq, mQ[k]);
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

    inline float processReject(float x) noexcept
    {
        float lpAcc = x;
        for (int k = 0; k < mSections; ++k)
        {
            float lo, b, h;
            mLp[k].process(lpAcc, lo, b, h);
            lpAcc = lo;
        }
        float hpAcc = x;
        for (int k = 0; k < mSections; ++k)
        {
            float lo, b, h;
            mHp[k].process(hpAcc, lo, b, h);
            hpAcc = h;
        }
        return lpAcc + hpAcc;
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
        bool   rejectL  = false;
        bool   rejectR  = false;
        float  mix      = 1.0f;
        float  agAmount[4]    = { 0.1f, 0.1f, 0.1f, 0.1f };
        double agPeriodSec[4] = { 1.0, 1.0, 1.0, 1.0 };
        bool   agEnableFreqL = false, agEnableBwL = false, agEnableGainL = false;
        bool   agEnableFreqR = false, agEnableBwR = false, agEnableGainR = false;
        bool   agEnableMix   = false;
        std::uint8_t agColorFreqL = 0, agColorBwL = 0, agColorGainL = 0;
        std::uint8_t agColorFreqR = 0, agColorBwR = 0, agColorGainR = 0;
        std::uint8_t agColorMix   = 0;
        double slopeDbL = 96.0;
        double slopeDbR = 96.0;
    };

    struct AgDeltas
    {
        float freqOct[2] = { 0.f, 0.f };
        float bwOct[2]   = { 0.f, 0.f };
        float gainDb[2]  = { 0.f, 0.f };
        float mix        = 0.f;
    };

    const AgDeltas& agDeltas() const noexcept { return mAgDeltas; }

    BandPassCore() = default;

    void prepare(double sampleRate) noexcept
    {
        mSampleRate = sampleRate;
        for (auto& ch : mCh) ch.prepare(sampleRate);
        for (int i = 0; i < kNumAgStreams; ++i)
        {
            mAgStream[i].walk.seed(0x9E3779B9u * (unsigned) (i + 1) + 0x5bd1e995u);
            mAgStream[i].walk.setPeriod(mAgStream[i].periodSec, sampleRate);
        }
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
        for (int c = 0; c < 4; ++c)
        {
            mParams.agAmount[c]    = std::clamp(p.agAmount[c], 0.0f, 1.0f);
            mParams.agPeriodSec[c] = std::max(p.agPeriodSec[c], 0.001);
        }
        if (p.linked)
        {
            mParams.freqR    = mParams.freqL;
            mParams.bwR      = mParams.bwL;
            mParams.slopeDbR = mParams.slopeDbL;
            mParams.gainR    = mParams.gainL;
            mParams.rejectR  = mParams.rejectL;
            mParams.agEnableFreqR = mParams.agEnableFreqL; mParams.agColorFreqR = mParams.agColorFreqL;
            mParams.agEnableBwR   = mParams.agEnableBwL;   mParams.agColorBwR   = mParams.agColorBwL;
            mParams.agEnableGainR = mParams.agEnableGainL; mParams.agColorGainR = mParams.agColorGainL;
        }
        const struct { bool en; std::uint8_t color; } maps[kNumAgStreams] = {
            { p.agEnableFreqL, p.agColorFreqL }, { p.agEnableBwL, p.agColorBwL }, { p.agEnableGainL, p.agColorGainL },
            { p.agEnableFreqR, p.agColorFreqR }, { p.agEnableBwR, p.agColorBwR }, { p.agEnableGainR, p.agColorGainR },
            { p.agEnableMix,   p.agColorMix },
        };
        for (int i = 0; i < kNumAgStreams; ++i)
        {
            const int col = std::min((int) maps[i].color, 3);
            const float amt = mParams.agAmount[col];
            mAgStream[i].active = maps[i].en && amt > 0.0f;
            mAgStream[i].amount = amt;
            mAgStream[i].periodSec = mParams.agPeriodSec[col];
            mAgStream[i].walk.setPeriod(mAgStream[i].periodSec, mSampleRate);
        }
        mCh[0].setTarget(mParams.freqL, mParams.bwL, mParams.slopeDbL);
        mCh[1].setTarget(mParams.freqR, mParams.bwR, mParams.slopeDbR);
        mCh[0].reject = mParams.rejectL;
        mCh[1].reject = mParams.rejectR;
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
    static constexpr double kAgFreqModOct  = 4.90689059560852;
    static constexpr double kAgBwModOct    = 4.64385618977472;
    static constexpr float  kAgGainModDb   = 48.f;
    static constexpr float  kAgMixMod      = 0.5f;
    static constexpr float  kDbToLin       = 0.115129254649702f;

    enum { kAgFreqL = 0, kAgBwL, kAgGainL, kAgFreqR, kAgBwR, kAgGainR, kAgMix, kNumAgStreams };

    class RandomWalk
    {
    public:
        void seed(unsigned s) noexcept
        {
            mState = s ? s : 1u;
            mFrom = mTo = mPhase = 0.0;
            mInc = 0.0;
        }
        void setPeriod(double seconds, double sampleRate) noexcept
        {
            mInc = 1.0 / std::max(seconds * sampleRate, 1.0);
        }
        inline float tick() noexcept
        {
            mPhase += mInc;
            if (mPhase >= 1.0)
            {
                mPhase -= std::floor(mPhase);
                mFrom = mTo;
                mTo = nextTarget();
            }
            const double t = 0.5 - 0.5 * std::cos(M_PI * mPhase);
            return static_cast<float>(mFrom + (mTo - mFrom) * t);
        }
    private:
        double nextTarget() noexcept
        {
            std::uint32_t x = mState;
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            mState = x;
            return (double) (x >> 8) / 8388607.5 - 1.0;
        }
        double mFrom = 0.0, mTo = 0.0, mPhase = 0.0, mInc = 0.0;
        std::uint32_t mState = 1;
    };

    struct AgStream
    {
        RandomWalk walk;
        bool active = false;
        float amount = 0.0f;
        double periodSec = 1.0;
    };
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
            chain.update(smFreq / halfBw, smFreq * halfBw, slopeDb, reject);
        }
        inline float process(float x, double modFreqOct, double modBwOct, double sampleRate) noexcept
        {
            if (modFreqOct != 0.0 || modBwOct != 0.0)
            {
                const double f = std::clamp(smFreq * std::exp2(modFreqOct), 20.0, sampleRate * 0.49);
                const double hb = halfBw * std::exp2(modBwOct * 0.5);
                chain.update(f / hb, f * hb, slopeDb, reject);
            }
            return reject ? chain.processReject(x) : chain.process(x);
        }
        double targetFreq = 1000.0, targetBw = 1.0;
        double smFreq = 1000.0, smBw = 1.0, halfBw = 1.0;
        double slopeDb = 96.0;
        bool   reject = false;
        ChannelChain chain;
    };

    void run(const float* const* in, float* const* out, float* const* wet,
             int numCh, int n) noexcept
    {
        const float gains[2] = { mParams.gainL, mParams.gainR };
        const bool mixMod = mAgStream[kAgMix].active;
        const float baseMix = mParams.mix;
        float dryGain, wetGain;
        if (!mixMod)
        {
            const float mixAngle = baseMix * 0.5f * static_cast<float>(M_PI);
            dryGain = std::cos(mixAngle);
            wetGain = std::sin(mixAngle);
        }
        else
        {
            dryGain = wetGain = 0.0f;
        }

        for (int i = 0; i < n; ++i)
        {
            float freqOct[2] = { 0.f, 0.f };
            float bwOct[2]   = { 0.f, 0.f };
            float gainDb[2]  = { 0.f, 0.f };
            float mixDelta   = 0.f;
            float gainLin[2] = { 1.f, 1.f };
            if (mAgStream[kAgFreqL].active)
                freqOct[0] = mAgStream[kAgFreqL].walk.tick() * mAgStream[kAgFreqL].amount * (float) kAgFreqModOct;
            if (mAgStream[kAgBwL].active)
                bwOct[0]   = mAgStream[kAgBwL].walk.tick() * mAgStream[kAgBwL].amount * (float) kAgBwModOct;
            if (mAgStream[kAgGainL].active)
            {
                gainDb[0]  = mAgStream[kAgGainL].walk.tick() * mAgStream[kAgGainL].amount * kAgGainModDb;
                gainLin[0] = std::exp(gainDb[0] * kDbToLin);
            }
            if (numCh > 1)
            {
                if (mParams.linked)
                {
                    freqOct[1] = freqOct[0];
                    bwOct[1]   = bwOct[0];
                    gainDb[1]  = gainDb[0];
                    gainLin[1] = gainLin[0];
                }
                else
                {
                    if (mAgStream[kAgFreqR].active)
                        freqOct[1] = mAgStream[kAgFreqR].walk.tick() * mAgStream[kAgFreqR].amount * (float) kAgFreqModOct;
                    if (mAgStream[kAgBwR].active)
                        bwOct[1]   = mAgStream[kAgBwR].walk.tick() * mAgStream[kAgBwR].amount * (float) kAgBwModOct;
                    if (mAgStream[kAgGainR].active)
                    {
                        gainDb[1]  = mAgStream[kAgGainR].walk.tick() * mAgStream[kAgGainR].amount * kAgGainModDb;
                        gainLin[1] = std::exp(gainDb[1] * kDbToLin);
                    }
                }
            }
            if (mixMod)
            {
                mixDelta = mAgStream[kAgMix].walk.tick() * mAgStream[kAgMix].amount * kAgMixMod;
                const float m = std::clamp(baseMix + mixDelta, 0.0f, 1.0f);
                const float mixAngle = m * 0.5f * static_cast<float>(M_PI);
                dryGain = std::cos(mixAngle);
                wetGain = std::sin(mixAngle);
            }
            for (int c = 0; c < numCh; ++c)
            {
                const float x = in[c][i];
                const float wetS = mCh[c].process(x, freqOct[c], bwOct[c], mSampleRate) * gains[c] * gainLin[c];
                if (wet[c]) wet[c][i] = wetS;
                out[c][i] = x * dryGain + wetS * wetGain;
            }
            mAgDeltas.freqOct[0] = freqOct[0];
            mAgDeltas.freqOct[1] = freqOct[1];
            mAgDeltas.bwOct[0]   = bwOct[0];
            mAgDeltas.bwOct[1]   = bwOct[1];
            mAgDeltas.gainDb[0]  = gainDb[0];
            mAgDeltas.gainDb[1]  = gainDb[1];
            mAgDeltas.mix        = mixDelta;
        }
    }

    double mSampleRate = 44100.0;
    Params mParams;
    Channel mCh[2];
    AgStream mAgStream[kNumAgStreams];
    AgDeltas mAgDeltas;
};

}
