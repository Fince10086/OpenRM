// VoiceRenderer.h — 短语回放器: 变调 (varispeed) + 门限包络
//
// Phase 1 的核心播放模型: note-on 时按当前参数离线渲染好的短语缓冲,
// 以 2^(note-baseKey)/12 的复速系数回放实现变调——复古采样机的玩法,
// 也是 SAM/TMS5220 这类芯片音色的正确打开方式。
// 纯 DSP 头, 零 iPlug2 依赖 (可离线测试)。
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace orm
{

class VoiceRenderer
{
public:
  // 装载一条已渲染短语 (单声道, sourceRate = 引擎原生采样率)
  void SetPhrase(const float* samples, int numSamples, double sourceRate, double hostRate)
  {
    mPhrase.assign(samples, samples + numSamples);
    mSrcRate = sourceRate;
    mHostRate = std::max(8000.0, hostRate);
    mNumSamples = numSamples;
    mPos = 0.0;
  }

  bool HasPhrase() const { return mNumSamples > 0; }
  bool IsPlaying() const { return mPlaying; }
  // 回放进度 0..1 (供 UI 时间线播放指针)
  double Progress() const
  {
    return mNumSamples > 0 ? std::min(1.0, mPos / mNumSamples) : 0.0;
  }

  void SetEnvelope(double attackMs, double releaseMs)
  {
    // 钳到 1ms 下限, 避免 0 速率
    mAttackMs = std::max(1.0, attackMs);
    mReleaseMs = std::max(1.0, releaseMs);
  }

  // pitchRatio = 2^((note - baseKey)/12)
  void Trigger(double pitchRatio)
  {
    pitchRatio = std::max(0.05, std::min(16.0, pitchRatio));
    mStep = pitchRatio * mSrcRate / mHostRate;
    mPos = 0.0;
    mEnv = 0.f;
    mEnvDir = 1; // 起音
    mPlaying = mNumSamples > 0;
  }

  // 进入释放段 (note-off): 若短语已自然结束则无事发生
  void Release()
  {
    if (mPlaying)
      mEnvDir = -1;
  }

  // 累加输出到 out (立体声插件放同一单声道信号即可)
  void ProcessAdd(float* out, int n)
  {
    if (!mPlaying || mNumSamples <= 0)
      return;

    const float attackInc = (float) (1000.0 / (mAttackMs * mHostRate));
    const float releaseDec = (float) (1000.0 / (mReleaseMs * mHostRate));
    const double maxPos = mNumSamples - 1;

    for (int i = 0; i < n; i++)
    {
      // 包络
      if (mEnvDir > 0)
      {
        mEnv += attackInc;
        if (mEnv >= 1.f)
        {
          mEnv = 1.f;
          mEnvDir = 0;
        }
      }
      else if (mEnvDir < 0)
      {
        mEnv -= releaseDec;
        if (mEnv <= 0.f)
        {
          mEnv = 0.f;
          mPlaying = false;
          return;
        }
      }

      // 线性插值回放 (varispeed)
      const int iPos = (int) mPos;
      const float frac = (float) (mPos - iPos);
      const float s0 = mPhrase[(size_t) iPos];
      const float s1 = mPhrase[(size_t) std::min(maxPos, (double) iPos + 1.0)];
      out[i] += (s0 + (s1 - s0) * frac) * mEnv;

      mPos += mStep;
      if (mPos >= maxPos)
      {
        mPlaying = false;
        return;
      }
    }
  }

private:
  std::vector<float> mPhrase;
  double mSrcRate = 22050.0;
  double mHostRate = 48000.0;
  double mPos = 0.0;
  double mStep = 1.0;
  int mNumSamples = 0;
  float mEnv = 0.f;
  int mEnvDir = 0; // 1 起音 / 0 保持 / -1 释放
  double mAttackMs = 5.0;
  double mReleaseMs = 80.0;
  bool mPlaying = false;
};

} // namespace orm
