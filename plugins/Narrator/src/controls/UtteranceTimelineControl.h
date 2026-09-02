#pragma once

#include "IControls.h"
#include "../Theme.h"
#include "../Params.h"

#include <algorithm>
#include <array>
#include <cmath>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

// 语句时间线: 渲染结果的最小/最大包络 + 播放进度指针。
// 波形数据由 delegate 在每次渲染后经 SendControlMsgFromDelegate 推送
// (kMsgTagPhrase: float 包络点数组, 中心对称的 min/max 值域 [-1,1]);
// 进度由 OnIdle 经 SetProgress() 轻量更新。
class UtteranceTimelineControl : public IControl
{
public:
  static constexpr int kMsgTagPhrase = 1;
  static constexpr int kEnvPoints = kPhraseEnvPoints;

  UtteranceTimelineControl(const IRECT &bounds)
      : IControl(bounds)
  {
  }

  // pData: kEnvPoints 个 float, 顺序包络 (正半轴幅值 0..1)
  void OnMsgFromDelegate(int msgTag, int dataSize, const void *pData) override
  {
    if (msgTag != kMsgTagPhrase || !pData)
      return;
    const int n = std::min((int) (dataSize / (int) sizeof(float)), kEnvPoints);
    if (n <= 0)
      return;
    for (int i = 0; i < n; i++)
      mEnv[(size_t) i] = static_cast<const float *>(pData)[i];
    mEnvCount = n;
    SetDirty(false);
  }

  void SetProgress(double progress, double durationSec)
  {
    mProgress = std::clamp(progress, 0.0, 1.0);
    if (durationSec > 0.0)
      mDuration = durationSec;
    SetDirty(false);
  }

  void SetDuration(double sec)
  {
    mDuration = sec;
    SetDirty(false);
  }

  void Draw(IGraphics &g) override
  {
    const IRECT b = mRECT;
    g.FillRect(COL_300(), b);

    const IRECT wave = b.GetPadded(-8.f);
    if (mEnvCount <= 1)
    {
      g.DrawText(IText(20, COL_700(), kFontRegular, EAlign::Center, EVAlign::Middle),
                 "--", b);
      return;
    }

    // 包络条
    const float bw = wave.W() / (float) mEnvCount;
    for (int i = 0; i < mEnvCount; i++)
    {
      const float a = std::min(1.f, std::max(0.f, mEnv[(size_t) i]));
      if (a <= 0.002f)
        continue;
      const float h = a * (wave.H() * 0.5f);
      const float cy = wave.MH();
      g.FillRect(COL_700(),
                 IRECT(wave.L + i * bw, cy - h, wave.L + (i + 1) * bw + 0.5f, cy + h));
    }

    // 播放进度线
    if (mProgress > 0.0)
    {
      const float px = wave.L + (float) mProgress * wave.W();
      g.FillRect(COL_900(), IRECT(px - 1.5f, wave.T, px + 1.5f, wave.B));
    }

    // 时长标签 (右上角)
    char buf[32];
    snprintf(buf, sizeof(buf), orm::Tr(orm::kTxtDuration, orm::UILang()), mDuration);
    g.DrawText(IText(14, COL_700(), kFontRegular, EAlign::Far, EVAlign::Top), buf, b.GetPadded(-6.f));
  }

private:
  std::array<float, kEnvPoints> mEnv{};
  int mEnvCount = 0;
  double mProgress = 0.0;
  double mDuration = 0.0;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
