// ============================================================================
// BandPassCore.h — 零依赖立体声带通滤波器核心 (GRM Tools BandPass 风格 v0.0.1)
//
// 算法: TPT State Variable Filter (Zavalishin / Cytomic)
//   每通道纯带通: BP(center/bw, 小球控制) -> gain
//   bandwidth: octave 带宽, 内部转 Q
//   BP 输出按 1/Q 归一化: 中心频率峰值增益恒为 ~1, 不随带宽变窄而抬升
// 特性:
//   - float32 处理路径, double 系数计算 (每 block 一次)
//   - 双声道独立 (dual-mono) / link 模式
//   - 一阶参数平滑防 zipper (时间常数与 buffer size 无关)
//   - agitation: 正弦 LFO 对中心频率调制 (强度/速率), 满强度 ±0.5 oct
//   - L->R / R->L / Flip 为参数数值拷贝/交换 (在插件壳层处理, 非 DSP 路由)
//   - mix 等功率干湿混合
//   - 状态极小值归零防 denormal
//   - 零依赖纯头文件, C++17
// ============================================================================
#pragma once

#include <cmath>
#include <algorithm>
#include <atomic>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

        // 防 denormal: 衰减到极小的状态归零 (float denormal 数会显著拖慢 CPU)
        if (std::fabs(mIc1eq) < 1e-20f) mIc1eq = 0.f;
        if (std::fabs(mIc2eq) < 1e-20f) mIc2eq = 0.f;
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
// 单写单读参数信箱 (seqlock): 编辑器线程 publish, 音频线程 consume
// consume 遇到写入中/撕裂时返回 false, 音频线程沿用上一块的参数即可
// ----------------------------------------------------------------------------
template <typename T>
class ParamMailbox
{
public:
    void publish(const T& p) noexcept
    {
        const unsigned v = mVersion.load(std::memory_order_relaxed);
        mVersion.store(v + 1, std::memory_order_relaxed);    // 进入写入
        std::atomic_thread_fence(std::memory_order_release);
        mSnapshot = p;
        mVersion.store(v + 2, std::memory_order_release);    // 写入完成
    }

    bool consume(T& out) noexcept
    {
        const unsigned v1 = mVersion.load(std::memory_order_acquire);
        if (v1 & 1u) return false;                           // 正在写入
        out = mSnapshot;
        std::atomic_thread_fence(std::memory_order_acquire);
        return v1 == mVersion.load(std::memory_order_relaxed);
    }

private:
    std::atomic<unsigned> mVersion {0};
    T mSnapshot {};
};

// ----------------------------------------------------------------------------
// 单通道滤波链: 纯带通 (TPT SVF 的 band 输出)
// ----------------------------------------------------------------------------
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
        mBandNorm = 1.0 / mQ;   // SVF band 输出峰值增益 = Q, 乘 1/Q 归一为 ~1
        bp.setParams(bpHz, mQ);
    }

    // agitation 调制时仅更新中心频率 (Q 与归一化系数不变)
    void setFreq(double bpHz) noexcept { bp.setParams(bpHz, mQ); }

    inline float process(float x) noexcept
    {
        float lo, b, h;
        bp.process(x, lo, b, h);
        return b * static_cast<float>(mBandNorm);   // 峰值增益归一化的带通输出
    }

    SvfFilter bp;

private:
    double mSampleRate = 44100.0;
    double mQ = 1.0;
    double mBandNorm = 1.0;
};

// ----------------------------------------------------------------------------
// 立体声 BandPass 核心
// ----------------------------------------------------------------------------
class BandPassCore
{
public:
    struct Params
    {
        // 每通道 (纯带通)
        double freqL = 1000.0;   // 中心频率 Hz 20..20000
        double bwL   = 1.0;      // 带宽 octave 0.05..4
        float  gainL = 1.0f;

        double freqR = 1000.0;
        double bwR   = 1.0;
        float  gainR = 1.0f;

        // 全局
        bool   linked   = false;
        float  mix      = 1.0f;   // 干湿 0..1
        bool   agOn     = false;  // agitation
        float  agAmount = 0.1f;   // 抖动强度 0..1
        double agRate   = 1.0;    // 抖动速率 Hz 0.05..20
    };

    BandPassCore() = default;

    void prepare(double sampleRate, int /*maxBlock*/) noexcept
    {
        mSampleRate = sampleRate;
        mChainL.prepare(sampleRate);
        mChainR.prepare(sampleRate);
        // 平滑状态快照
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
        // 一阶平滑, 时间常数 15ms, 系数按实际块长换算 (与 buffer size 无关)
        const double coef = 1.0 - std::exp(-(double)blockSize / (0.015 * mSampleRate));
        mSmFreqL += (mParams.freqL - mSmFreqL) * coef;
        mSmBwL   += (mParams.bwL   - mSmBwL)   * coef;

        if (mParams.linked)
        {
            // link: R 以相同方式滑向 L 的目标值 (而非瞬间贴上 L 的当前值)
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

    // 立体声处理 (支持每声道原位 in==out)
    void process(const float* const inL, const float* const inR,
                 float* const outL, float* const outR, int n) noexcept
    {
        const float gL = mParams.gainL, gR = mParams.gainR;
        // 等功率干湿交叉淡化
        const float mixAngle = mParams.mix * 0.5f * static_cast<float>(M_PI);
        const float dryGain = std::cos(mixAngle), wetGain = std::sin(mixAngle);
        const float agAmt = mParams.agOn ? mParams.agAmount : 0.0f;
        const double agInc = 2.0 * M_PI * mParams.agRate / mSampleRate;

        for (int i = 0; i < n; ++i)
        {
            // agitation: 正弦 LFO 以 octave 为单位调制中心频率 (满强度 ±0.5 oct)
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
                // 抖动时按样本重算 BP 系数 (仅频率, Q 不变)
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

    // 单声道
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
    static constexpr double kAgitationMaxOct = 0.5;  // 满强度时中心频率摆动 ±0.5 oct

    double mSampleRate = 44100.0;
    Params mParams;
    ChannelChain mChainL, mChainR;
    double mAgPhase = 0.0;

    // 平滑后的参数状态
    double mSmFreqL = 1000.0, mSmBwL = 1.0;
    double mSmFreqR = 1000.0, mSmBwR = 1.0;
};

} // namespace grm
